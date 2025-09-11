#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdint.h>

#define PULL mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

uint8_t *rom, *chrrom, prg[4], chr[8], prgbits = 14, chrbits = 12,
    A, X, Y, P = 4, S = ~2, PCH, PCL, addr_lo, addr_hi, nomem, result, val,
    cross, tmp, ppumask, ppuctrl, ppustatus, ppubuf, W, fine_x, opcode, nmi_irq,
    ntb, ptb_lo, vram[2048], palette_ram[64], ram[8192], chrram[8192],
    prgram[8192], oam[256], mask[] = {128, 64, 1, 2, 1, 0, 0, 1, 4, 0, 0, 4, 0, 0, 64, 0, 8, 0, 0, 8},
    keys, mirror, mmc1_bits, mmc1_data, mmc1_ctrl, mmc3_chrprg[8], mmc3_bits,
    mmc3_irq, mmc3_latch, chrbank0, chrbank1, prgbank, rombuf[1024 * 1024],
    *key_state;

// 추가: APU 관련 변수
uint8_t apu_status = 0, frame_counter = 0, frame_seq = 0; // $4015, $4017, 시퀀서
uint16_t pulse1_freq = 0, pulse2_freq = 0, tri_freq = 0, noise_freq = 0; // 주파수
uint8_t pulse1_duty = 0, pulse2_duty = 0, pulse1_vol = 0, pulse2_vol = 0,
        tri_vol = 0, noise_vol = 0, pulse1_env = 0, pulse2_env = 0, noise_env = 0,
        pulse1_len = 0, pulse2_len = 0, tri_len = 0, noise_len = 0,
        pulse1_sweep = 0, pulse2_sweep = 0, tri_linear = 0,
        dmc_freq = 0, dmc_load = 0, dmc_addr = 0, dmc_len = 0, dmc_buffer = 0,
        dmc_bits = 0, dmc_output = 0;
uint16_t pulse1_phase = 0, pulse2_phase = 0, tri_phase = 0, noise_shift = 1;
uint32_t apu_cycles = 0; // APU 타이밍 추적
int16_t apu_sample = 0; // 현재 샘플
SDL_AudioSpec audio_spec;
//uint8_t audio_buffer[1024];
uint8_t audio_buffer[2048];
uint16_t audio_pos = 0;

// 추가: APU 테이블
const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};
const uint16_t noise_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};
const uint16_t dmc_rates[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54
};
const uint8_t triangle_wave[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
const uint8_t duty_wave[4][8] = {
    {0,0,0,0,0,0,0,1}, {0,0,0,0,0,0,1,1}, {0,0,0,0,1,1,1,1}, {1,1,1,1,1,1,0,0}
};

uint16_t scany, T, V, sum, dot, atb, shift_hi, shift_lo, cycles, frame_buffer[61440];
int shift_at = 0;

uint8_t *get_chr_byte(uint16_t a) {
  return &chrrom[chr[a >> chrbits] << chrbits | a % (1 << chrbits)];
}

uint8_t *get_nametable_byte(uint16_t a) {
  return &vram[mirror == 0 ? a % 1024 : mirror == 1 ? a % 1024 + 1024 : mirror == 2 ? a & 2047 : a / 2 & 1024 | a % 1024];
}

uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write) {
  uint16_t addr = hi << 8 | lo;

  switch (hi >>= 4) {
  case 0: case 1: return write ? ram[addr] = val : ram[addr];

  case 2: case 3: // PPU
    lo &= 7;
    if (lo == 7) {
      tmp = ppubuf;
      uint8_t *rom = V < 8192 ? write && chrrom != chrram ? &tmp : get_chr_byte(V)
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
      case 6: T = (W ^= 1) ? T & 0xff | val % 64 << 8 : (V = T & ~0xff | val); }
    if (lo == 2) { tmp = ppustatus & 0xe0; ppustatus &= 0x7f; W = 0; return tmp; }
    break;

  case 4:
  
    if (write && lo == 20)
      for (uint16_t i = 256; i--;)
        oam[i] = mem(i, val, 0, 0);
    for (tmp = 0, hi = 8; hi--;)
      tmp = tmp * 2 + key_state[(uint8_t[]){
                          SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_TAB, SDL_SCANCODE_RETURN,
                          SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT}[hi]];
    if (lo == 22) {
      if (write) { keys = tmp; } else { tmp = keys & 1; keys /= 2; return tmp; }
    }

    if (addr >= 0x4000 && addr <= 0x4017) { // 추가: APU 레지스터
      if (write) {
        switch (addr & 0x1F) {
          case 0x00: pulse1_duty = val >> 6; pulse1_env = val & 0x0F; pulse1_vol = (val & 0x10) ? pulse1_env : val & 0x0F; break;
          case 0x01: pulse1_sweep = val; break;
          case 0x02: pulse1_freq = (pulse1_freq & 0x0700) | val; break;
          case 0x03: pulse1_freq = (val & 0x07) << 8 | (pulse1_freq & 0xFF); pulse1_len = length_table[val >> 3]; pulse1_vol = (pulse1_sweep & 0x10) ? pulse1_env : pulse1_env; pulse1_phase = 0; break;
          case 0x04: pulse2_duty = val >> 6; pulse2_env = val & 0x0F; pulse2_vol = (val & 0x10) ? pulse2_env : val & 0x0F; break;
          case 0x05: pulse2_sweep = val; break;
          case 0x06: pulse2_freq = (pulse2_freq & 0x0700) | val; break;
          case 0x07: pulse2_freq = (val & 0x07) << 8 | (pulse2_freq & 0xFF); pulse2_len = length_table[val >> 3]; pulse2_vol = (pulse2_sweep & 0x10) ? pulse2_env : pulse2_env; pulse2_phase = 0; break;
          case 0x08: tri_linear = val & 0x7F; tri_vol = val & 0x7F; break;
          case 0x0A: tri_freq = (tri_freq & 0x0700) | val; break;
          case 0x0B: tri_freq = (val & 0x07) << 8 | (tri_freq & 0xFF); tri_len = length_table[val >> 3]; tri_phase = 0; break;
          case 0x0C: noise_env = val & 0x0F; noise_vol = (val & 0x10) ? noise_env : val & 0x0F; break;
          case 0x0E: noise_freq = val & 0x0F; noise_shift = 1; break;
          case 0x0F: noise_len = length_table[val >> 3]; noise_vol = (noise_env & 0x10) ? noise_env : noise_env; noise_shift = 1; break;
          case 0x10: dmc_freq = val & 0x0F; if (val & 0x80) nmi_irq |= 2; else nmi_irq &= ~2; break;
          case 0x11: dmc_load = val & 0x7F; dmc_output = dmc_load; break;
          case 0x12: dmc_addr = val; break;
          case 0x13: dmc_len = val; break;
          case 0x15:
            apu_status = val;
            if (!(val & 1)) pulse1_len = 0;
            if (!(val & 2)) pulse2_len = 0;
            if (!(val & 4)) tri_len = 0;
            if (!(val & 8)) noise_len = 0;
            if (!(val & 16)) { dmc_len = 0; dmc_bits = 0; } else if (!dmc_len) { dmc_len = dmc_len; dmc_addr = dmc_addr; dmc_bits = 8; }
            break;
          case 0x17: frame_counter = val; frame_seq = 0; if (val & 0x80) apu_cycles = 0; break;
        }
      } else if (addr == 0x4015) {
        tmp = (pulse1_len > 0) | (pulse2_len > 0) << 1 | (tri_len > 0) << 2 | (noise_len > 0) << 3 | (dmc_len > 0) << 4 | (nmi_irq & 2) << 7;
        nmi_irq &= ~2;
        return tmp;
      }
      return 0;
    }

    return 0;

  case 6: case 7: addr &= 8191; return write ? prgram[addr] = val : prgram[addr];

  default:
    if (write)
      switch (rombuf[6] >> 4) {
      case 7: mirror = !(val / 16); prg[0] = val % 8 * 2; prg[1] = prg[0] + 1; break;
      case 4: {
        uint8_t addr1 = addr & 1;
        switch (hi >> 1) {
          case 4: *(addr1 ? &mmc3_chrprg[mmc3_bits & 7] : &mmc3_bits) = val;
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
        if (val & 0x80) {
          mmc1_bits = 5; mmc1_data = 0; mmc1_ctrl |= 12;
        } else if (mmc1_data = mmc1_data / 2 | val << 4 & 16, !--mmc1_bits) {
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

uint8_t read_pc() { val = mem(PCL, PCH, 0, 0); !++PCL && ++PCH; return val; }
uint8_t set_nz(uint8_t val) { return P = P & 125 | val & 128 | !val * 2; }

// 추가: APU 업데이트 함수
void update_apu() {
  apu_cycles++;
  // 프레임 시퀀서 (NTSC: 1.789773 MHz CPU 클록 기준)
  uint8_t step = frame_counter & 0x80 ? 5 : 4;
  uint32_t frame_cycles = step == 4 ? 7457 : 7458;
  if (apu_cycles >= frame_cycles) {
    apu_cycles = 0;
    frame_seq++;
    if (frame_seq >= step) frame_seq = 0;
    // 엔벨로프 및 길이 카운터 업데이트
    if ((frame_counter & 0x40) == 0 || frame_seq == step - 1) {
      // 엔벨로프 decay
      if (pulse1_vol && !(pulse1_sweep & 0x10) && --pulse1_vol == 0) pulse1_vol = pulse1_env;
      if (pulse2_vol && !(pulse2_sweep & 0x10) && --pulse2_vol == 0) pulse2_vol = pulse2_env;
      if (noise_vol && !(noise_env & 0x10) && --noise_vol == 0) noise_vol = noise_env;
      // 길이 카운터
      if (pulse1_len && !(pulse1_sweep & 0x20)) pulse1_len--;
      if (pulse2_len && !(pulse2_sweep & 0x20)) pulse2_len--;
      if (tri_len && !(tri_linear & 0x80)) tri_len--;
      if (noise_len && !(noise_env & 0x20)) noise_len--;
    }
    // 스윕 유닛
    if (frame_seq % 2 == 0) {
      if (pulse1_sweep & 0x80 && pulse1_freq >= 8) {
        uint16_t shift = (pulse1_sweep & 0x07) ? pulse1_freq >> (pulse1_sweep & 0x07) : 0;
        pulse1_freq += (pulse1_sweep & 0x08) ? -shift : shift;
        if (pulse1_freq > 0x7FF) pulse1_freq = 0;
      }
      if (pulse2_sweep & 0x80 && pulse2_freq >= 8) {
        uint16_t shift = (pulse2_sweep & 0x07) ? pulse2_freq >> (pulse2_sweep & 0x07) : 0;
        pulse2_freq += (pulse2_sweep & 0x08) ? -shift : shift;
        if (pulse2_freq > 0x7FF) pulse2_freq = 0;
      }
    }
  }

  // 채널 샘플 생성 (44100Hz 기준)
  int16_t sample = 0;
  // 펄스 1
  if (apu_status & 1 && pulse1_len && pulse1_freq >= 8) {
    pulse1_phase += (1789773.0 / 44100.0); // CPU 클록 기준 위상 증가
    if (duty_wave[pulse1_duty][(pulse1_phase / (pulse1_freq + 1)) % 8]) sample += pulse1_vol * 8;
  }
  // 펄스 2
  if (apu_status & 2 && pulse2_len && pulse2_freq >= 8) {
    pulse2_phase += (1789773.0 / 44100.0);
    if (duty_wave[pulse2_duty][(pulse2_phase / (pulse2_freq + 1)) % 8]) sample += pulse2_vol * 8;
  }
  // 삼각파
  if (apu_status & 4 && tri_len && tri_vol && tri_freq >= 2) {
    tri_phase += (1789773.0 / 44100.0);
    sample += triangle_wave[(tri_phase / (tri_freq + 1)) % 32] * 4;
  }
  // 노이즈
  if (apu_status & 8 && noise_len) {
    static uint16_t noise_timer = 0;
    if (--noise_timer == 0) {
      noise_timer = noise_periods[noise_freq];
      uint8_t feedback = (noise_shift & 1) ^ ((noise_shift >> (noise_freq & 0x80 ? 6 : 1)) & 1);
      noise_shift = (noise_shift >> 1) | (feedback << 14);
    }
    if (noise_shift & 1) sample += noise_vol * 8;
  }
  // DMC
  if (apu_status & 16 && dmc_len) {
    static uint16_t dmc_timer = 0;
    if (--dmc_timer == 0) {
      dmc_timer = dmc_rates[dmc_freq];
      if (dmc_bits == 0) {
        if (dmc_len) {
          dmc_buffer = mem(dmc_addr, 0xC0, 0, 0); // $C000-$FFFF에서 읽기
          dmc_addr = (dmc_addr + 1) & 0x3FFF;
          if (--dmc_len == 0 && (dmc_freq & 0x40)) { dmc_len = dmc_len; dmc_addr = dmc_addr; }
          dmc_bits = 8;
        }
      }
      if (dmc_bits) {
        if (dmc_buffer & 1) {
          if (dmc_output <= 125) dmc_output += 2;
        } else {
          if (dmc_output >= 2) dmc_output -= 2;
        }
        dmc_buffer >>= 1;
        dmc_bits--;
      }
    }
    sample += dmc_output;
  }
  apu_sample = sample / 5; // 간단한 믹싱
}

// 추가: SDL 오디오 콜백
void audio_callback(void *userdata, Uint8 *stream, int len) {
  for (int i = 0; i < len; i++) {
    update_apu(); // 샘플마다 호출 (44100Hz)
    stream[i] = (apu_sample + 128); // 8-bit unsigned
  }
}

int main(int argc, char **argv) {
  SDL_RWread(SDL_RWFromFile(argv[1], "rb"), rombuf, 1024 * 1024, 1);
  rom = rombuf + 16;
  prg[1] = rombuf[4] - 1;
  chrrom = rombuf[5] ? rom + ((prg[1] + 1) << 14) : chrram;
  chr[1] = (rombuf[5] || 1) * 2 - 1;
  mirror = 3 - rombuf[6] % 2;
  if (rombuf[6] / 16 == 4) {
    mem(0, 128, 0, 1);
    prgbits--;
    chrbits -= 2;
  }
  PCL = mem(~3, ~0, 0, 0);
  PCH = mem(~2, ~0, 0, 0);

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
  key_state = (uint8_t*)SDL_GetKeyboardState(0);
  // 추가: 오디오 초기화
  audio_spec.freq = 44100;
  audio_spec.format = AUDIO_U8;
  audio_spec.channels = 1;
  audio_spec.samples = 1024;
  audio_spec.callback = audio_callback;
  SDL_OpenAudio(&audio_spec, NULL);
  SDL_PauseAudio(0);

  void *renderer = SDL_CreateRenderer(
      SDL_CreateWindow("smolnes", 0, 0, 1024, 840, SDL_WINDOW_SHOWN), -1,
      SDL_RENDERER_PRESENTVSYNC);
  void *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGR565,
                                    SDL_TEXTUREACCESS_STREAMING, 256, 224);

loop:
  cycles = nomem = 0;
  if (nmi_irq) goto nmi_irq;

  opcode = read_pc();
  uint8_t opcodelo5 = opcode & 31;
  switch (opcodelo5) {
  case 0:
    if (opcode & 0x80) { read_pc(); nomem = 1; goto nomemop; }
    switch (opcode >> 5) {
    case 0: {
      !++PCL && ++PCH;
    nmi_irq:
      PUSH(PCH); PUSH(PCL); PUSH(P | 32);
      uint16_t veclo = ~1 - (nmi_irq & 4);
      PCL = mem(veclo, ~0, 0, 0);
      PCH = mem(veclo + 1, ~0, 0, 0);
      nmi_irq = 0;
      cycles++;
      break;
    }
    case 1: result = read_pc(); PUSH(PCH); PUSH(PCL); PCH = read_pc(); PCL = result; break;
    case 2: P = PULL & ~32; PCL = PULL; PCH = PULL; break;
    case 3: PCL = PULL; PCH = PULL; !++PCL && ++PCH; break;
    }
    cycles += 4;
    break;

  case 16: read_pc(); if (!(P & mask[opcode >> 6]) ^ opcode / 32 & 1) {
    cross = PCL + (int8_t)val >> 8; PCH += cross; PCL += val; cycles += cross ? 2 : 1; } break;

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
    default: nomem = 1; val = A; goto nomemop;
    }
    break;

  case 1: read_pc(); val += X; addr_lo = mem(val, 0, 0, 0); addr_hi = mem(val + 1, 0, 0, 0); cycles += 4; goto opcode;
  case 2: case 9: read_pc(); nomem = 1; goto nomemop;
  case 17: addr_lo = mem(read_pc(), 0, 0, 0); addr_hi = mem(val + 1, 0, 0, 0); cycles++; goto add_x_or_y;
  case 4: case 5: case 6: case 20: case 21: case 22:
    addr_lo = read_pc(); cross = opcodelo5 > 6; if (cross) addr_lo += (opcode & 214) == 150 ? Y : X; addr_hi = 0; cycles -= !cross; goto opcode;
  case 12: case 13: case 14: case 25: case 28: case 29: case 30:
    addr_lo = read_pc(); addr_hi = read_pc(); if (opcodelo5 < 25) goto opcode;
  add_x_or_y:
    val = opcodelo5 < 28 | opcode == 190 ? Y : X; cross = addr_lo + val > 255; addr_hi += cross; addr_lo += val;
    cycles += ((opcode & 224) == 128 | opcode % 16 == 14 & opcode != 190) | cross;
  opcode:
    cycles += 2;
    if (opcode != 76 & (opcode & 224) != 128) val = mem(addr_lo, addr_hi, 0, 0);
  nomemop:
    result = 0;
    switch (opcode & 227) {
    case 1: set_nz(A |= val); break;
    case 33: set_nz(A &= val); break;
    case 65: set_nz(A ^= val); break;
    case 225: val = ~val;
    case 97: sum = A + val + P % 2; P = P & ~65 | sum > 255 | ((A ^ sum) & (val ^ sum) & 128) / 2; set_nz(A = sum); break;
    case 34: result = P & 1;
    case 2: result |= val * 2; P = P & ~1 | val / 128; goto memop;
    case 98: result = P << 7;
    case 66: result |= val / 2; P = P & ~1 | val & 1; goto memop;
    case 194: result = val - 1; goto memop;
    case 226: result = val + 1;
    memop: set_nz(result); nomem ? A = result : (cycles += 2, mem(addr_lo, addr_hi, result, 1)); break;
    case 32: P = P & 61 | val & 192 | !(A & val) * 2; break;
    case 64: PCL = addr_lo; PCH = addr_hi; cycles--; break;
    case 96: PCL = val; PCH = mem(addr_lo + 1, addr_hi, 0, 0); cycles++; break;
    default: {
      uint8_t opcodehi3 = opcode / 32;
      uint8_t *reg = opcode % 4 == 2 | opcodehi3 == 7 ? &X : opcode % 4 == 1 ? &A : &Y;
      if (opcodehi3 == 4) mem(addr_lo, addr_hi, *reg, 1);
      else if (opcodehi3 != 5) { P = P & ~1 | *reg >= val; set_nz(*reg - val); }
      else set_nz(*reg = val);
      break;
    }
    }
  }

  // PPU 및 APU 업데이트
  for (tmp = cycles * 3 + 6; tmp--;) {
    // 추가: APU 업데이트 (CPU 사이클당 호출)
    update_apu();
    if (ppumask & 24) {
      if (scany < 240) {
        if (dot - 256 > 63u) {
          if (dot < 256) {
            uint8_t color = shift_hi >> 14 - fine_x & 2 | shift_lo >> 15 - fine_x & 1,
                    palette = shift_at >> 28 - fine_x * 2 & 12;
            if (ppumask & 16) {
              for (uint8_t *sprite = oam; sprite < oam + 256; sprite += 4) {
                uint16_t sprite_h = ppuctrl & 32 ? 16 : 8, sprite_x = dot - sprite[3], sprite_y = scany - sprite[0] - 1,
                         sx = sprite_x ^ !(sprite[2] & 64) * 7, sy = sprite_y ^ (sprite[2] & 128 ? sprite_h - 1 : 0);
                if (sprite_x < 8 && sprite_y < sprite_h) {
                  uint16_t sprite_tile = sprite[1],
                           sprite_addr = (ppuctrl & 32 ? sprite_tile % 2 << 12 | sprite_tile << 4 & -32 | sy * 2 & 16 : (ppuctrl & 8) << 9 | sprite_tile << 4) | sy & 7,
                           sprite_color = *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 | *get_chr_byte(sprite_addr) >> sx & 1;
                  if (sprite_color) {
                    if (!(sprite[2] & 32 && color)) {
                      color = sprite_color; palette = 16 | sprite[2] * 4 & 12;
                    }
                    if (sprite == oam && color) ppustatus |= 64;
                    break;
                  }
                }
              }
            }
            frame_buffer[scany * 256 + dot] = (uint16_t[64]){
                25356, 34816, 39011, 30854, 24714, 4107, 106, 2311, 2468, 2561, 4642, 6592, 20832, 0, 0, 0,
                44373, 49761, 55593, 51341, 43186, 18675, 434, 654, 4939, 5058, 3074, 19362, 37667, 0, 0, 0,
                ~0, ~819, 64497, 64342, 62331, 43932, 23612, 9465, 1429, 1550, 20075, 36358, 52713, 16904, 0, 0,
                ~0, ~328, ~422, ~452, ~482, 58911, 50814, 42620, 40667, 40729, 48951, 53078, 61238, 44405}
                [palette_ram[color ? palette | color : 0]];
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
            shift_hi |= ptb_hi; shift_lo |= ptb_lo; shift_at |= atb; break;
          }
          }
        }
        if (dot == 256) {
          V = ((V & 7 << 12) != 7 << 12 ? V + 4096 : (V & 0x3e0) == 928 ? V & 0x8c1f ^ 2048 : (V & 0x3e0) == 0x3e0 ? V & 0x8c1f : V & 0x8c1f | V + 32 & 0x3e0) & ~0x41f | T & 0x41f;
        }
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
        for (SDL_Event event; SDL_PollEvent(&event);)
          if (event.type == SDL_QUIT) return 0;
      }
      if (scany == 261) ppustatus = 0;
    }
    if (++dot == 341) { dot = 0; scany++; scany %= 262; }
  }
  goto loop;
}