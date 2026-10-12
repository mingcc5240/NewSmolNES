#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PULL mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

// ==========================================
// smolnes 에뮬레이터 전역 상태 변수
// ==========================================
static uint8_t *rom, *chrrom,
    prg[4], chr[8],
    prgbits = 14, chrbits = 12,
    A, X, Y, P = 4, S = ~2, PC_H, PCL,
    addr_lo, addr_hi,
    nomem, result, val, cross, tmp,
    ppumask, ppuctrl, ppustatus,
    ppubuf, W, fine_x, opcode,
    nmi_irq, ntb, ptb_lo,
    vram[2048],
    palette_ram[64],
    ram[8192],
    chrram[8192],
    prgram[8192],
    oam[256],
    mask[] = {128, 64, 1, 2, 1, 0, 0, 1, 4, 0, 0, 4, 0, 0, 64, 0, 8, 0, 0, 8},
    keys,
    mirror,
    mmc1_bits, mmc1_data, mmc1_ctrl,
    mmc3_chrprg[8], mmc3_bits,
    mmc3_irq, mmc3_latch,
    chrbank0, chrbank1, prgbank,
    rombuf[1024 * 1024];

static uint16_t scany, T, V, sum, dot, atb, shift_hi, shift_lo, cycles;
static int shift_at = 0;

// VB6 외부 주입용 키 상태 (A, B, Select, Start, Up, Down, Left, Right)
static uint8_t vb_key_state = 0;

// VB6 DIB Section 호환 32bpp XRGB 버퍼 (256 x 240)
static uint32_t frame_buffer[256 * 240];

// NES 64색 RGB 팔레트 (0x00RRGGBB 포맷)
static const uint32_t nes_palette32[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFFFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFFFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000
};

// ==========================================
// smolnes 메모리 및 버스 제어
// ==========================================
static uint8_t *get_chr_byte(uint16_t a) {
    return &chrrom[chr[a >> chrbits] << chrbits | a % (1 << chrbits)];
}

static uint8_t *get_nametable_byte(uint16_t a) {
    return &vram[mirror == 0   ? a % 1024
                 : mirror == 1 ? a % 1024 + 1024
                 : mirror == 2 ? a & 2047
                               : a / 2 & 1024 | a % 1024];
}

static uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write) {
    uint16_t addr = hi << 8 | lo;

    switch (hi >>= 4) {
    case 0: case 1:
        return write ? ram[addr] = val : ram[addr];

    case 2: case 3:
        lo &= 7;
        if (lo == 7) {
            tmp = ppubuf;
            uint8_t *rom =
                V < 8192 ? (write && chrrom != chrram ? &tmp : get_chr_byte(V))
                : V < 16128 ? get_nametable_byte(V)
                : palette_ram + (uint8_t)((V & 19) == 16 ? V ^ 16 : V);
            write ? *rom = val : (ppubuf = *rom);
            V += ppuctrl & 4 ? 32 : 1;
            V %= 16384;
            return tmp;
        }

        if (write) {
            switch (lo) {
            case 0:
                ppuctrl = val;
                T = T & 0xf3ff | val % 4 << 10;
                break;
            case 1:
                ppumask = val;
                break;
            case 5:
                T = (W ^= 1)
                    ? fine_x = val & 7, T & ~31 | val / 8
                    : T & 0x8c1f | val % 8 << 12 | val * 4 & 0x3e0;
                break;
            case 6:
                T = (W ^= 1)
                    ? T & 0xff | val % 64 << 8
                    : (V = T & ~0xff | val);
                break;
            }
        }

        if (lo == 2) {
            tmp = ppustatus & 0xe0;
            ppustatus &= 0x7f;
            W = 0;
            return tmp;
        }
        break;

    case 4:
        if (write && lo == 20) {
            for (uint16_t i = 256; i--;)
                oam[i] = mem(i, val, 0, 0);
        }
        
        // $4016 컨트롤러 스트로브 및 읽기 (VB6에서 주입된 키 매핑)
        if (lo == 22) {
            if (write) {
                keys = vb_key_state;
            } else {
                tmp = keys & 1;
                keys /= 2;
                return tmp;
            }
        }
        return 0;

    case 6: case 7:
        addr &= 8191;
        return write ? prgram[addr] = val : prgram[addr];

    default:
        if (write) {
            switch (rombuf[6] >> 4) {
            case 7:
                mirror = !(val / 16);
                prg[0] = val % 8 * 2;
                prg[1] = prg[0] + 1;
                break;
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
                case 5:
                    if (!addr1) mirror = 2 + val % 2;
                    break;
                case 6:
                    if (!addr1) mmc3_latch = val;
                    break;
                case 7:
                    mmc3_irq = addr1;
                    break;
                }
                break;
            }
            case 3:
                chr[0] = val % 4 * 2;
                chr[1] = chr[0] + 1;
                break;
            case 2:
                prg[0] = val & 31;
                break;
            case 1:
                if (val & 0x80) {
                    mmc1_bits = 5;
                    mmc1_data = 0;
                    mmc1_ctrl |= 12;
                } else if (mmc1_data = mmc1_data / 2 | val << 4 & 16, !--mmc1_bits) {
                    mmc1_bits = 5;
                    tmp = addr >> 13;
                    *(tmp == 4 ? mirror = mmc1_data & 3, &mmc1_ctrl
                      : tmp == 5 ? &chrbank0
                      : tmp == 6 ? &chrbank1
                                 : &prgbank) = mmc1_data;

                    chr[0] = chrbank0 & ~!(mmc1_ctrl & 16);
                    chr[1] = mmc1_ctrl & 16 ? chrbank1 : chrbank0 | 1;

                    tmp = mmc1_ctrl / 4 % 4 - 2;
                    prg[0] = !tmp ? 0 : tmp == 1 ? prgbank : prgbank & ~1;
                    prg[1] = !tmp ? prgbank : tmp == 1 ? rombuf[4] - 1 : prgbank | 1;
                }
            }
        }
        return rom[(prg[hi - 8 >> prgbits - 12] & (rombuf[4] << 14 - prgbits) - 1)
                   << prgbits |
                   addr & (1 << prgbits) - 1];
    }
    return ~0;
}

static uint8_t read_pc() {
    val = mem(PCL, PC_H, 0, 0);
    !++PCL && ++PC_H;
    return val;
}

static uint8_t set_nz(uint8_t val) {
    return P = P & 125 | val & 128 | !val * 2;
}

// ==========================================
// VB6 노출용 __stdcall API 인터페이스
// ==========================================

__declspec(dllexport) int __stdcall NES_LoadROM(const char* filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return 0;

    memset(rombuf, 0, sizeof(rombuf));
    size_t read_bytes = fread(rombuf, 1, sizeof(rombuf), fp);
    fclose(fp);

    if (read_bytes < 16 || memcmp(rombuf, "NES\x1A", 4) != 0) {
        return 0;
    }

    rom = rombuf + 16;
    prg[1] = rombuf[4] - 1;
    chrrom = rombuf[5] ? rom + (rombuf[4] << 14) : chrram;
    chr[1] = rombuf[5] ? rombuf[5] * 2 - 1 : 1;
    mirror = 3 - rombuf[6] % 2;

    prgbits = 14;
    chrbits = 12;

    if (rombuf[6] / 16 == 4) {
        mem(0, 128, 0, 1);
        prgbits--;
        chrbits -= 2;
    }

    P = 4;
    S = ~2;
    PCL = mem(~3, ~0, 0, 0);
    PC_H = mem(~2, ~0, 0, 0);

    dot = 0;
    scany = 0;
    nmi_irq = 0;
    W = 0;

    return 1;
}

__declspec(dllexport) void __stdcall NES_SetInput(uint8_t buttons) {
    vb_key_state = buttons;
}

__declspec(dllexport) uint32_t* __stdcall NES_GetBuffer(void) {
    return frame_buffer;
}

__declspec(dllexport) void __stdcall NES_RunFrame(void) {
    int frame_done = 0;

    while (!frame_done) {
        cycles = nomem = 0;
        if (nmi_irq) goto nmi_irq_entry;

        opcode = read_pc();
        uint8_t opcodelo5 = opcode & 31;
        switch (opcodelo5) {
        case 0:
            if (opcode & 0x80) {
                read_pc();
                nomem = 1;
                goto nomemop_entry;
            }

            switch (opcode >> 5) {
            case 0: {
                !++PCL && ++PC_H;
            nmi_irq_entry:
                PUSH(PC_H);
                PUSH(PCL);
                PUSH(P | 32);
                uint16_t veclo = ~1 - (nmi_irq & 4);
                PCL = mem(veclo, ~0, 0, 0);
                PC_H = mem(veclo + 1, ~0, 0, 0);
                nmi_irq = 0;
                cycles++;
                break;
            }
            case 1:
                result = read_pc();
                PUSH(PC_H);
                PUSH(PCL);
                PC_H = read_pc();
                PCL = result;
                break;
            case 2:
                P = PULL & ~32;
                PCL = PULL;
                PC_H = PULL;
                break;
            case 3:
                PCL = PULL;
                PC_H = PULL;
                !++PCL && ++PC_H;
                break;
            }
            cycles += 4;
            break;

        case 16:
            read_pc();
            if (!(P & mask[opcode >> 6]) ^ opcode / 32 & 1) {
                cross = PCL + (int8_t)val >> 8;
                PC_H += cross;
                PCL += val;
                cycles += cross ? 2 : 1;
            }
            break;

        case 8: case 24:
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

        case 10: case 26:
            switch (opcode >> 4) {
            case 8: set_nz(A = X); break;
            case 9: S = X; break;
            case 10: set_nz(X = A); break;
            case 11: set_nz(X = S); break;
            case 12: set_nz(--X); break;
            case 14: break;
            default: nomem = 1; val = A; goto nomemop_entry;
            }
            break;

        case 1:
            read_pc();
            val += X;
            addr_lo = mem(val, 0, 0, 0);
            addr_hi = mem(val + 1, 0, 0, 0);
            cycles += 4;
            goto opcode_entry;

        case 2: case 9:
            read_pc();
            nomem = 1;
            goto nomemop_entry;

        case 17:
            addr_lo = mem(read_pc(), 0, 0, 0);
            addr_hi = mem(val + 1, 0, 0, 0);
            cycles++;
            goto add_x_or_y_entry;

        case 4: case 5: case 6:
        case 20: case 21: case 22:
            addr_lo = read_pc();
            cross = opcodelo5 > 6;
            if (cross) addr_lo += (opcode & 214) == 150 ? Y : X;
            addr_hi = 0;
            cycles -= !cross;
            goto opcode_entry;

        case 12: case 13: case 14:
        case 25:
        case 28: case 29: case 30:
            addr_lo = read_pc();
            addr_hi = read_pc();
            if (opcodelo5 < 25) goto opcode_entry;
        add_x_or_y_entry:
            val = opcodelo5 < 28 | opcode == 190 ? Y : X;
            cross = addr_lo + val > 255;
            addr_hi += cross;
            addr_lo += val;
            cycles += ((opcode & 224) == 128 | opcode % 16 == 14 & opcode != 190) | cross;
        opcode_entry:
            cycles += 2;
            if (opcode != 76 & (opcode & 224) != 128) {
                val = mem(addr_lo, addr_hi, 0, 0);
            }

        nomemop_entry:
            result = 0;
            switch (opcode & 227) {
            case 1: set_nz(A |= val); break;
            case 33: set_nz(A &= val); break;
            case 65: set_nz(A ^= val); break;
            case 225: val = ~val;
            case 97:
                sum = A + val + P % 2;
                P = P & ~65 | sum > 255 | ((A ^ sum) & (val ^ sum) & 128) / 2;
                set_nz(A = sum);
                break;
            case 34: result = P & 1;
            case 2:
                result |= val * 2;
                P = P & ~1 | val / 128;
                goto memop_entry;
            case 98: result = P << 7;
            case 66:
                result |= val / 2;
                P = P & ~1 | val & 1;
                goto memop_entry;
            case 194:
                result = val - 1;
                goto memop_entry;
            case 226:
                result = val + 1;
            memop_entry:
                set_nz(result);
                nomem ? A = result : (cycles += 2, mem(addr_lo, addr_hi, result, 1));
                break;
            case 32:
                P = P & 61 | val & 192 | !(A & val) * 2;
                break;
            case 64:
                PCL = addr_lo;
                PC_H = addr_hi;
                cycles--;
                break;
            case 96:
                PCL = val;
                PC_H = mem(addr_lo + 1, addr_hi, 0, 0);
                cycles++;
                break;
            default: {
                uint8_t opcodehi3 = opcode / 32;
                uint8_t *reg = opcode % 4 == 2 | opcodehi3 == 7 ? &X
                               : opcode % 4 == 1 ? &A : &Y;
                if (opcodehi3 == 4) {
                    mem(addr_lo, addr_hi, *reg, 1);
                } else if (opcodehi3 != 5) {
                    P = P & ~1 | *reg >= val;
                    set_nz(*reg - val);
                } else {
                    set_nz(*reg = val);
                }
                break;
            }
            }
        }

        // PPU 클럭 처리
        for (tmp = cycles * 3 + 6; tmp--;) {
            if (ppumask & 24) {
                if (scany < 240) {
                    if (dot - 256 > 63u) {
                        if (dot < 256) {
                            uint8_t color = shift_hi >> 14 - fine_x & 2 |
                                            shift_lo >> 15 - fine_x & 1,
                                    palette = shift_at >> 28 - fine_x * 2 & 12;

                            if (ppumask & 16) {
                                for (uint8_t *sprite = oam; sprite < oam + 256; sprite += 4) {
                                    uint16_t sprite_h = ppuctrl & 32 ? 16 : 8,
                                             sprite_x = dot - sprite[3],
                                             sprite_y = scany - sprite[0] - 1,
                                             sx = sprite_x ^ !(sprite[2] & 64) * 7,
                                             sy = sprite_y ^ (sprite[2] & 128 ? sprite_h - 1 : 0);
                                    if (sprite_x < 8 && sprite_y < sprite_h) {
                                        uint16_t sprite_tile = sprite[1],
                                                 sprite_addr =
                                                     (ppuctrl & 32
                                                          ? sprite_tile % 2 << 12 |
                                                            sprite_tile << 4 & -32 | sy * 2 & 16
                                                          : (ppuctrl & 8) << 9 | sprite_tile << 4) |
                                                     sy & 7,
                                                 sprite_color =
                                                     *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 |
                                                     *get_chr_byte(sprite_addr) >> sx & 1;
                                        if (sprite_color) {
                                            if (!(sprite[2] & 32 && color)) {
                                                color = sprite_color;
                                                palette = 16 | sprite[2] * 4 & 12;
                                            }
                                            if (sprite == oam && color)
                                                ppustatus |= 64;
                                            break;
                                        }
                                    }
                                }
                            }

                            // 32비트 XRGB 색상 기록
                            frame_buffer[scany * 256 + dot] =
                                nes_palette32[palette_ram[color ? palette | color : 0] & 0x3F];
                        }

                        if (dot < 336) {
                            shift_hi *= 2;
                            shift_lo *= 2;
                            shift_at *= 4;
                        }

                        int temp = ppuctrl << 8 & 4096 | ntb << 4 | V >> 12;
                        switch (dot & 7) {
                        case 1: ntb = *get_nametable_byte(V); break;
                        case 3:
                            atb = (*get_nametable_byte(V & 0xc00 | 0x3c0 | V >> 4 & 0x38 |
                                                       V / 4 & 7) >>
                                   (V >> 5 & 2 | V / 2 & 1) * 2) %
                                  4 * 0x5555;
                            break;
                        case 5: ptb_lo = *get_chr_byte(temp); break;
                        case 7: {
                            uint8_t ptb_hi = *get_chr_byte(temp | 8);
                            V = V % 32 == 31 ? V & ~31 ^ 1024 : V + 1;
                            shift_hi |= ptb_hi;
                            shift_lo |= ptb_lo;
                            shift_at |= atb;
                            break;
                        }
                        }
                    }

                    if (dot == 256) {
                        V = ((V & 7 << 12) != 7 << 12 ? V + 4096
                             : (V & 0x3e0) == 928     ? V & 0x8c1f ^ 2048
                             : (V & 0x3e0) == 0x3e0   ? V & 0x8c1f
                                                      : V & 0x8c1f | V + 32 & 0x3e0) &
                            ~0x41f | T & 0x41f;
                    }
                }

                if ((scany + 1) % 262 < 241 && dot == 261 && mmc3_irq && !mmc3_latch--)
                    nmi_irq = 1;

                if (scany == 261 && dot - 280 < 25u)
                    V = V & 0x841f | T & 0x7be0;
            }

            if (dot == 1) {
                if (scany == 241) {
                    if (ppuctrl & 128)
                        nmi_irq = 4;
                    ppustatus |= 128;
                    // VBlank 시작: 1프레임 렌더링 완료 플래그 활성화
                    frame_done = 1;
                }
                if (scany == 261)
                    ppustatus = 0;
            }

            if (++dot == 341) {
                dot = 0;
                scany++;
                scany %= 262;
            }
        }
    }
}