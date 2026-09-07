#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PULL mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

uint8_t *rom, *chrrom,                // Points to the start of PRG/CHR ROM
    prg[4], chr[8],                   // Current PRG/CHR banks
    prgbits = 14, chrbits = 12,       // Number of bits per PRG/CHR bank
    A, X, Y, P = 4, S = ~2, PCH, PCL, // CPU Registers
    addr_lo, addr_hi,                 // Current instruction address
    nomem,  // 1 => current instruction doesn't write to memory
    result, // Temp variable
    val,    // Current instruction value
    cross,  // 1 => page crossing occurred
    tmp,    // Temp variables
    ppumask, ppuctrl, ppustatus, // PPU registers
    ppubuf,                      // PPU buffered reads
    W,                           // Write toggle PPU register
    fine_x,                      // X fine scroll offset, 0..7
    opcode,                      // Current instruction opcode
    nmi_irq,                     // 1 => IRQ occurred / 4 => NMI occurred
    ntb,                         // Nametable byte
    ptb_lo,                      // Pattern table lowbyte
    vram[2048],                  // Nametable RAM
    palette_ram[64],             // Palette RAM
    ram[8192],                   // CPU RAM
    chrram[8192],                // CHR RAM
    prgram[8192],                // PRG RAM
    oam[256],                    // Object Attribute Memory
    mask[] = {128, 64, 1, 2, 1, 0, 0, 1, 4, 0, 0, 4, 0, 0, 64, 0, 8, 0, 0, 8},
    keys,                        // Joypad shift register
    mirror,                      // Current mirroring mode
    mmc1_bits, mmc1_data, mmc1_ctrl,   // Mapper 1 (MMC1) registers
    mmc3_chrprg[8], mmc3_bits,         // Mapper 4 (MMC3) registers
    mmc3_irq, mmc3_latch,              
    chrbank0, chrbank1, prgbank,       // Current PRG/CHR bank
    rombuf[1024 * 1024],               // Buffer to read ROM file into
    *key_state;

uint16_t scany,          // Scanline Y
    T, V,                // "Loopy" PPU registers
    sum,                 // Sum used for ADC/SBC
    dot,                 // Horizontal position of PPU, from 0..340
    atb,                 // Attribute byte
    shift_hi, shift_lo,  // Pattern table shift registers
    cycles,              // Cycle count for current instruction
    frame_buffer[61440]; // Frame buffer

int shift_at = 0;

// === APU (Audio) Registers & Variables ===
uint8_t apu_regs[0x20];
double p1_phase = 0, p2_phase = 0;
uint16_t noise_shift = 1;

// SDL Audio Callback Function
void audio_callback(void *userdata, uint8_t *stream, int len) {
    int16_t *buffer = (int16_t *)stream;
    int samples = len / 2;

    uint8_t p1_ctrl = apu_regs[0x00], p1_lo = apu_regs[0x02], p1_hi = apu_regs[0x03];
    uint8_t p2_ctrl = apu_regs[0x04], p2_lo = apu_regs[0x06], p2_hi = apu_regs[0x07];
    uint8_t noise_ctrl = apu_regs[0x0C];
    uint8_t apu_enable = apu_regs[0x15];

    // Pulse 1 Frequency & Volume
    uint16_t p1_timer = p1_lo | ((p1_hi & 0x07) << 8);
    double p1_freq = (p1_timer > 7 && (apu_enable & 1)) ? 1789773.0 / (16.0 * (p1_timer + 1)) : 0;
    uint8_t p1_vol = p1_ctrl & 0x0F;

    // Pulse 2 Frequency & Volume
    uint16_t p2_timer = p2_lo | ((p2_hi & 0x07) << 8);
    double p2_freq = (p2_timer > 7 && (apu_enable & 2)) ? 1789773.0 / (16.0 * (p2_timer + 1)) : 0;
    uint8_t p2_vol = p2_ctrl & 0x0F;

    // Noise Volume
    uint8_t noise_vol = (apu_enable & 4) ? (noise_ctrl & 0x0F) : 0;

    for (int i = 0; i < samples; i++) {
        int16_t sample = 0;

        // Pulse 1 Synthesis
        if (p1_freq > 0) {
            p1_phase += p1_freq / 44100.0;
            if (p1_phase >= 1.0) p1_phase -= 1.0;
            sample += (p1_phase < 0.5 ? 1 : -1) * (p1_vol * 150);
        }

        // Pulse 2 Synthesis
        if (p2_freq > 0) {
            p2_phase += p2_freq / 44100.0;
            if (p2_phase >= 1.0) p2_phase -= 1.0;
            sample += (p2_phase < 0.5 ? 1 : -1) * (p2_vol * 150);
        }

        // Noise Channel Synthesis
        if (noise_vol > 0) {
            uint8_t feedback = ((noise_shift & 1) ^ ((noise_shift >> 1) & 1));
            noise_shift = (noise_shift >> 1) | (feedback << 14);
            sample += (noise_shift & 1 ? 1 : -1) * (noise_vol * 100);
        }

        buffer[i] = sample;
    }
}

// Read a byte from CHR ROM or CHR RAM.
uint8_t *get_chr_byte(uint16_t a) {
    return &chrrom[chr[a >> chrbits] << chrbits | a % (1 << chrbits)];
}

// Read a byte from nametable RAM.
uint8_t *get_nametable_byte(uint16_t a) {
    return &vram[mirror == 0   ? a % 1024
                 : mirror == 1 ? a % 1024 + 1024
                 : mirror == 2 ? a & 2047
                               : a / 2 & 1024 | a % 1024];
}

// Memory R/W function with APU Memory Mapping
uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write) {
    uint16_t addr = hi << 8 | lo;
    switch (hi >>= 4) {
    case 0:
    case 1: // $0000...$1fff RAM
        return write ? ram[addr] = val : ram[addr];
    case 2:
    case 3: // $2000..$2007 PPU (mirrored)
        lo &= 7;
        if (lo == 7) {
            tmp = ppubuf;
            uint8_t *rom =
                V < 8192 ? write && chrrom != chrram ? &tmp : get_chr_byte(V)
                : V < 16128 ? get_nametable_byte(V)
                            : palette_ram + (uint8_t)((V & 19) == 16 ? V ^ 16 : V);
            write ? *rom = val : (ppubuf = *rom);
            V += ppuctrl & 4 ? 32 : 1;
            V %= 16384;
            return tmp;
        }
        if (write)
            switch (lo) {
            case 0: ppuctrl = val; T = T & 0xf3ff | val % 4 << 10; break;
            case 1: ppumask = val; break;
            case 5: T = (W ^= 1) ? fine_x = val & 7, T & ~31 | val / 8 : T & 0x8c1f | val % 8 << 12 | val * 4 & 0x3e0; break;
            case 6: T = (W ^= 1) ? T & 0xff | val % 64 << 8 : (V = T & ~0xff | val);
            }
        if (lo == 2) {
            tmp = ppustatus & 0xe0;
            ppustatus &= 0x7f;
            W = 0;
            return tmp;
        }
        break;
    case 4:
        if (write && lo == 20) // $4014 OAM DMA
            for (uint16_t i = 256; i--;) oam[i] = mem(i, val, 0, 0);

        if (lo == 22) { // $4016 Joypad 1
            if (write) {
                for (tmp = 0, hi = 8; hi--;)
                    tmp = tmp * 2 + key_state[(uint8_t[]){
                                        SDL_SCANCODE_X,      // A
                                        SDL_SCANCODE_Z,      // B
                                        SDL_SCANCODE_TAB,    // Select
                                        SDL_SCANCODE_RETURN, // Start
                                        SDL_SCANCODE_UP,     // Dpad Up
                                        SDL_SCANCODE_DOWN,   // Dpad Down
                                        SDL_SCANCODE_LEFT,   // Dpad Left
                                        SDL_SCANCODE_RIGHT   // Dpad Right
                                    }[hi]];
                keys = tmp;
            } else {
                tmp = keys & 1;
                keys /= 2;
                return tmp;
            }
        }

        // APU Sound Registers Handling ($4000 ~ $4017)
        if (lo < 0x20) {
            if (write) apu_regs[lo] = val;
            return apu_regs[lo];
        }
        return 0;
    case 6:
    case 7: // $6000...$7fff PRG RAM
        addr &= 8191;
        return write ? prgram[addr] = val : prgram[addr];
    default: // $8000...$ffff ROM Mappers
        if (write)
            switch (rombuf[6] >> 4) {
            case 7: mirror = !(val / 16); prg[0] = val % 8 * 2; prg[1] = prg[0] + 1; break;
            case 4: {
                uint8_t addr1 = addr & 1;
                switch (hi >> 1) {
                case 4:
                    *(addr1 ? &mmc3_chrprg[mmc3_bits & 7] : &mmc3_bits) = val;
                    tmp = mmc3_bits >> 5 & 4;
                    for (int i = 4; i--;) {
                        chr[0 + i + tmp] = mmc3_chrprg[i / 2] & ~!(i % 2) | i % 2;
                        chr[4 + i - tmp] = mmc3_chrprg[2 + i];
                    }
                    tmp = mmc3_bits >> 5 & 2;
                    prg[0 + tmp] = mmc3_chrprg[6];
                    prg[1] = mmc3_chrprg[7];
                    prg[3] = rombuf[4] * 2 - 1;
                    prg[2 - tmp] = prg[3] - 1;
                    break;
                case 5: if (!addr1) mirror = 2 + val % 2; break;
                case 6: if (!addr1) mmc3_latch = val; break;
                case 7: mmc3_irq = addr1; break;
                }
                break;
            }
            case 3: chr[0] = val % 4 * 2; chr[1] = chr[0] + 1; break;
            case 2: prg[0] = val & 31; break;
            case 1:
                if (val & 0x80) { mmc1_bits = 5; mmc1_data = 0; mmc1_ctrl |= 12; }
                else if (mmc1_data = mmc1_data / 2 | val << 4 & 16, !--mmc1_bits) {
                    mmc1_bits = 5;
                    tmp = addr >> 13;
                    *(tmp == 4 ? mirror = mmc1_data & 3, &mmc1_ctrl : tmp == 5 ? &chrbank0 : tmp == 6 ? &chrbank1 : &prgbank) = mmc1_data;
                    chr[0] = chrbank0 & ~!(mmc1_ctrl & 16);
                    chr[1] = mmc1_ctrl & 16 ? chrbank1 : chrbank0 | 1;
                    tmp = mmc1_ctrl / 4 % 4 - 2;
                    prg[0] = !tmp ? 0 : tmp == 1 ? prgbank : prgbank & ~1;
                    prg[1] = !tmp ? prgbank : tmp == 1 ? rombuf[4] - 1 : prgbank | 1;
                }
            }
        return rom[(prg[hi - 8 >> prgbits - 12] & (rombuf[4] << 14 - prgbits) - 1) << prgbits | addr & (1 << prgbits) - 1];
    }
    return ~0;
}

uint8_t read_pc() {
    val = mem(PCL, PCH, 0, 0);
    !++PCL && ++PCH;
    return val;
}

uint8_t set_nz(uint8_t val) {
    return P = P & 125 | val & 128 | !val * 2;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <rom_file.nes>\n", argv[0]);
        return 1;
    }

   FILE *f = fopen(argv[1], "rb");
if (!f) {
    printf("Error: Could not open ROM file '%s'\n", argv[1]);
    return 1;
}
fread(rombuf, 1, 1024 * 1024, f);
fclose(f);

    // === iNES Header 검증 및 Mapper 추출 ===
    uint8_t mapper = (rombuf[6] >> 4) | (rombuf[7] & 0xF0);

    // smolnes가 지원하는 Mapper 확인 (0, 1, 2, 3, 4, 7)
    if (mapper != 0 && mapper != 1 && mapper != 2 && mapper != 3 && mapper != 4 && mapper != 7) {
        printf("[Error] Unsupported iNES Mapper: %d\n", mapper);
        printf("This emulator only supports Mapper 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), and 7 (AxROM).\n");
        printf("Exiting program.\n");
        return 1;
    }

    printf("Loaded ROM successfully. Mapper: %d\n", mapper);

    rom = rombuf + 16;
    prg[1] = rombuf[4] - 1;
    chrrom = rombuf[5] ? rom + (rombuf[4] << 14) : chrram;
    chr[1] = rombuf[5] ? rombuf[5] * 2 - 1 : 1;
    mirror = 3 - rombuf[6] % 2;
    if (rombuf[6] / 16 == 4) {
        mem(0, 128, 0, 1);
        prgbits--;
        chrbits -= 2;
    }

    PCL = mem(~3, ~0, 0, 0);
    PCH = mem(~2, ~0, 0, 0);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    // === SDL Audio Initialization ===
    SDL_AudioSpec wanted_spec = {
        .freq = 44100,
        .format = AUDIO_S16SYS,
        .channels = 1,
        .samples = 512,
        .callback = audio_callback
    };
    SDL_OpenAudio(&wanted_spec, NULL);
    SDL_PauseAudio(0);

    key_state = (uint8_t*)SDL_GetKeyboardState(0);

    void *renderer = SDL_CreateRenderer(SDL_CreateWindow("smolnes+Audio+mapper print", 10, 30, 1024, 840, SDL_WINDOW_SHOWN), -1, SDL_RENDERER_PRESENTVSYNC);
    void *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGR565, SDL_TEXTUREACCESS_STREAMING, 256, 224);

loop:
    cycles = nomem = 0;
    if (nmi_irq) goto nmi_irq;
    opcode = read_pc();
    uint8_t opcodelo5 = opcode & 31;
    switch (opcodelo5) {
    case 0:
        if (opcode & 0x80) { read_pc(); nomem = 1; goto nomemop; }
        switch (opcode >> 5) {
        case 0:
            !++PCL && ++PCH;
        nmi_irq:
            PUSH(PCH); PUSH(PCL); PUSH(P | 32);
            uint16_t veclo = ~1 - (nmi_irq & 4);
            PCL = mem(veclo, ~0, 0, 0);
            PCH = mem(veclo + 1, ~0, 0, 0);
            nmi_irq = 0; cycles++; break;
        case 1: result = read_pc(); PUSH(PCH); PUSH(PCL); PCH = read_pc(); PCL = result; break;
        case 2: P = PULL & ~32; PCL = PULL; PCH = PULL; break;
        case 3: PCL = PULL; PCH = PULL; !++PCL && ++PCH; break;
        }
        cycles += 4; break;
    case 16:
        read_pc();
        if (!(P & mask[opcode >> 6]) ^ opcode / 32 & 1) {
            cross = PCL + (int8_t)val >> 8;
            PCH += cross; PCL += val;
            cycles += cross ? 2 : 1;
        }
        break;
    case 8:
    case 24:
        switch (opcode >>= 4) {
        case 0: PUSH(P | 48); cycles++; break;
        case 2: P = PULL & ~16; cycles += 2; break;
        case 4: PUSH(A); cycles++; break;
        case 6: set_nz(A = PULL); cycles += 2; break;
        case 8: set_nz(--Y); break;
        case 9: set_nz(A = Y); break;
        case 10: set_nz(Y = A); break;
        case 12: set_nz(++Y); break;
        case 14: set_nz(++X); break;
        default: P = P & ~mask[opcode + 3] | mask[opcode + 4]; break;
        }
        break;
    case 10:
    case 26:
        switch (opcode >> 4) {
        case 8: set_nz(A = X); break;
        case 9: S = X; break;
        case 10: set_nz(X = A); break;
        case 11: set_nz(X = S); break;
        case 12: set_nz(--X); break;
        case 14: break;
        default: nomem = 1; val = A; goto nomemop;
        }
        break;
    case 1:
        read_pc(); val += X;
        addr_lo = mem(val, 0, 0, 0);
        addr_hi = mem(val + 1, 0, 0, 0);
        cycles += 4; goto opcode;
    case 2:
    case 9:
        read_pc(); nomem = 1; goto nomemop;
    case 17:
        addr_lo = mem(read_pc(), 0, 0, 0);
        addr_hi = mem(val + 1, 0, 0, 0);
        cycles++; goto add_x_or_y;
    case 4: case 5: case 6:
    case 20: case 21: case 22:
        addr_lo = read_pc();
        cross = opcodelo5 > 6;
        if (cross) { addr_lo += (opcode & 214) == 150 ? Y : X; }
        addr_hi = 0; cycles -= !cross; goto opcode;
    case 12: case 13: case 14:
    case 25: case 28: case 29: case 30:
        addr_lo = read_pc(); addr_hi = read_pc();
        if (opcodelo5 < 25) goto opcode;
    add_x_or_y:
        val = opcodelo5 < 28 | opcode == 190 ? Y : X;
        cross = addr_lo + val > 255;
        addr_hi += cross; addr_lo += val;
        cycles += ((opcode & 224) == 128 | opcode % 16 == 14 & opcode != 190) | cross;
    opcode:
        cycles += 2;
        if (opcode != 76 & (opcode & 224) != 128) { val = mem(addr_lo, addr_hi, 0, 0); }
    nomemop:
        result = 0;
        switch (opcode & 227) {
        case 1: set_nz(A |= val); break;
        case 33: set_nz(A &= val); break;
        case 65: set_nz(A ^= val); break;
        case 225: val = ~val;
        case 97:
            sum = A + val + P % 2;
            P = P & ~65 | sum > 255 | ((A ^ sum) & (val ^ sum) & 128) / 2;
            set_nz(A = sum); break;
        case 34: result = P & 1;
        case 2: result |= val * 2; P = P & ~1 | val / 128; goto memop;
        case 98: result = P << 7;
        case 66: result |= val / 2; P = P & ~1 | val & 1; goto memop;
        case 194: result = val - 1; goto memop;
        case 226: result = val + 1;
        memop:
            set_nz(result);
            nomem ? A = result : (cycles += 2, mem(addr_lo, addr_hi, result, 1)); break;
        case 32: P = P & 61 | val & 192 | !(A & val) * 2; break;
        case 64: PCL = addr_lo; PCH = addr_hi; cycles--; break;
        case 96: PCL = val; PCH = mem(addr_lo + 1, addr_hi, 0, 0); cycles++; break;
        default: {
            uint8_t opcodehi3 = opcode / 32;
            uint8_t *reg = opcode % 4 == 2 | opcodehi3 == 7 ? &X : opcode % 4 == 1 ? &A : &Y;
            if (opcodehi3 == 4) { mem(addr_lo, addr_hi, *reg, 1); }
            else if (opcodehi3 != 5) { P = P & ~1 | *reg >= val; set_nz(*reg - val); }
            else { set_nz(*reg = val); }
            break;
        }
        }
    }

    // PPU Loop
    for (tmp = cycles * 3 + 6; tmp--;) {
        if (ppumask & 24) {
            if (scany < 240) {
                if (dot - 256 > 63u) {
                    if (dot < 256) {
                        uint8_t color = shift_hi >> 14 - fine_x & 2 | shift_lo >> 15 - fine_x & 1, palette = shift_at >> 28 - fine_x * 2 & 12;
                        if (ppumask & 16) {
                            for (uint8_t *sprite = oam; sprite < oam + 256; sprite += 4) {
                                uint16_t sprite_h = ppuctrl & 32 ? 16 : 8, sprite_x = dot - sprite[3], sprite_y = scany - sprite[0] - 1, sx = sprite_x ^ !(sprite[2] & 64) * 7, sy = sprite_y ^ (sprite[2] & 128 ? sprite_h - 1 : 0);
                                if (sprite_x < 8 && sprite_y < sprite_h) {
                                    uint16_t sprite_tile = sprite[1], sprite_addr = (ppuctrl & 32 ? sprite_tile % 2 << 12 | sprite_tile << 4 & -32 | sy * 2 & 16 : (ppuctrl & 8) << 9 | sprite_tile << 4) | sy & 7, sprite_color = *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 | *get_chr_byte(sprite_addr) >> sx & 1;
                                    if (sprite_color) {
                                        if (!(sprite[2] & 32 && color)) { color = sprite_color; palette = 16 | sprite[2] * 4 & 12; }
                                        if (sprite == oam && color) ppustatus |= 64;
                                        break;
                                    }
                                }
                            }
                        }
                        frame_buffer[scany * 256 + dot] = (uint16_t[64]){25356, 34816, 39011, 30854, 24714, 4107, 106, 2311, 2468, 2561, 4642, 6592, 20832, 0, 0, 0, 44373, 49761, 55593, 51341, 43186, 18675, 434, 654, 4939, 5058, 3074, 19362, 37667, 0, 0, 0, ~0, ~819, 64497, 64342, 62331, 43932, 23612, 9465, 1429, 1550, 20075, 36358, 52713, 16904, 0, 0, ~0, ~328, ~422, ~452, ~482, 58911, 50814, 42620, 40667, 40729, 48951, 53078, 61238, 44405}[palette_ram[color ? palette | color : 0]];
                    }
                    if (dot < 336) { shift_hi *= 2; shift_lo *= 2; shift_at *= 4; }
                    int temp = ppuctrl << 8 & 4096 | ntb << 4 | V >> 12;
                    switch (dot & 7) {
                    case 1: ntb = *get_nametable_byte(V); break;
                    case 3: atb = (*get_nametable_byte(V & 0xc00 | 0x3c0 | V >> 4 & 0x38 | V / 4 & 7) >> (V >> 5 & 2 | V / 2 & 1) * 2) % 4 * 0x5555; break;
                    case 5: ptb_lo = *get_chr_byte(temp); break;
                    case 7: {
                        uint8_t ptb_hi = *get_chr_byte(temp | 8);
                        V = V % 32 == 31 ? V & ~31 ^ 1024 : V + 1;
                        shift_hi |= ptb_hi; shift_lo |= ptb_lo; shift_at |= atb;
                        break;
                    }
                    }
                }
                if (dot == 256) { V = ((V & 7 << 12) != 7 << 12 ? V + 4096 : (V & 0x3e0) == 928 ? V & 0x8c1f ^ 2048 : (V & 0x3e0) == 0x3e0 ? V & 0x8c1f : V & 0x8c1f | V + 32 & 0x3e0) & ~0x41f | T & 0x41f; }
            }
            if ((scany + 1) % 262 < 241 && dot == 261 && mmc3_irq && !mmc3_latch--) nmi_irq = 1;
            if (scany == 261 && dot - 280 < 25u) V = V & 0x841f | T & 0x7be0;
        }
        if (dot == 1) {
            if (scany == 241) {
                if (ppuctrl & 128) nmi_irq = 4;
                ppustatus |= 128;
                SDL_UpdateTexture(texture, 0, frame_buffer + 2048, 512);
                SDL_RenderCopy(renderer, texture, 0, 0);
                SDL_RenderPresent(renderer);
                for (SDL_Event event; SDL_PollEvent(&event);) if (event.type == SDL_QUIT) return 0;
            }
            if (scany == 261) ppustatus = 0;
        }
        if (++dot == 341) { dot = 0; scany++; scany %= 262; }
    }
    goto loop;
}