#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PULL mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

// [추가] mem 함수 전방 선언 (인자 4개)
static uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write);
static uint16_t cur_mapper = 0;
// ========================================================
// 오디오 파라미터 및 Win32 waveOut 동적 바인딩
// ========================================================
#define AUDIO_SAMPLE_RATE 44100
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 60) // 735 샘플
#define AUDIO_NUM_BUFFERS 4                        // 4중 버퍼로 언더런(끊김) 완전 방지

//#define WAVE_MAPPER     ((unsigned int)-1)
#define WAVE_FORMAT_PCM 1
#define MMSYSERR_NOERROR 0
//#define WHDR_DONE       0x00000001

//typedef void *HWAVEOUT;


typedef unsigned int (__stdcall *pfn_waveOutOpen)(void**, unsigned int, const WAVEFORMATEX*, unsigned long, unsigned long, unsigned long);
typedef unsigned int (__stdcall *pfn_waveOutPrepareHeader)(void*, WAVEHDR*, unsigned int);
typedef unsigned int (__stdcall *pfn_waveOutUnprepareHeader)(void*, WAVEHDR*, unsigned int);
typedef unsigned int (__stdcall *pfn_waveOutWrite)(void*, WAVEHDR*, unsigned int);
typedef unsigned int (__stdcall *pfn_waveOutReset)(void*);
typedef unsigned int (__stdcall *pfn_waveOutClose)(void*);

static pfn_waveOutOpen fn_waveOutOpen = NULL;
static pfn_waveOutPrepareHeader fn_waveOutPrepareHeader = NULL;
static pfn_waveOutUnprepareHeader fn_waveOutUnprepareHeader = NULL;
static pfn_waveOutWrite fn_waveOutWrite = NULL;
static pfn_waveOutReset fn_waveOutReset = NULL;
static pfn_waveOutClose fn_waveOutClose = NULL;
static HMODULE h_winmm_mod = NULL;
static HWAVEOUT h_wave_out = NULL;

static int16_t audio_buffers[AUDIO_NUM_BUFFERS][SAMPLES_PER_FRAME];
static WAVEHDR wave_hdrs[AUDIO_NUM_BUFFERS];
static int audio_buf_idx = 0;

// ========================================================
// APU 채널 정의 (Pulse 1, Pulse 2, Triangle, Noise)
// ========================================================
typedef struct {
    uint8_t enabled;
    uint16_t timer_period;
    double phase;
    uint8_t duty;
    uint8_t volume;
    uint8_t length_counter;
    uint8_t constant_volume;
} PulseChannel;

typedef struct {
    uint8_t enabled;
    uint16_t timer_period;
    double phase;
    uint8_t length_counter;
    uint8_t linear_counter;
} TriangleChannel;

typedef struct {
    uint8_t enabled;
    uint16_t timer_period;
    uint16_t shift_reg;     // 15비트 LFSR 시프트 레지스터
    double phase;
    uint8_t volume;
    uint8_t length_counter;
    uint8_t mode;           // 0: 32767비트 긴 주기, 1: 93비트 짧은 주기
} NoiseChannel;

typedef struct {
    uint8_t  enabled;
    uint8_t  loop;
    uint8_t  irq_enabled;
    uint16_t rate_period;
    uint16_t sample_address;    // $C000 + (A * 64)
    uint16_t sample_length;     // (L * 16) + 1
    uint16_t current_address;
    uint16_t bytes_remaining;
    uint8_t  sample_buffer;
    uint8_t  shift_reg;
    uint8_t  bits_remaining;
    uint8_t  output_level;      // 0 ~ 127 DAC 카운터
    uint8_t  buffer_empty;
    double   phase;
} DMCChannel;

static PulseChannel pulse1, pulse2;
static TriangleChannel triangle;
static NoiseChannel noise;
static DMCChannel      dmc;

// 노이즈 주파수 타이머 룩업 테이블 (NTSC 표준)
static const uint16_t noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

// DMC 주파수 주기 룩업 테이블 (NTSC CPU 클럭 주기)
static const uint16_t dmc_period_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106,  84,  72,  54
};

// 사각파 Duty 테이블
static const uint8_t duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}
};

// 삼각파 시퀀스 (0~15~0)
static const int8_t triangle_table[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};


// ========================================================
// DMC 샘플 버퍼 페치 및 내부 클럭 처리
// ========================================================
static void dmc_fetch_byte(void) {
    if (dmc.bytes_remaining > 0 && dmc.buffer_empty) {
        dmc.sample_buffer = mem((uint8_t)(dmc.current_address & 0xFF), (uint8_t)(dmc.current_address >> 8), 0,0);
        dmc.buffer_empty = 0;
        dmc.current_address++;
        if (dmc.current_address == 0) dmc.current_address = 0x8000;
        dmc.bytes_remaining--;
        if (dmc.bytes_remaining == 0 && dmc.loop) {
            dmc.current_address = dmc.sample_address;
            dmc.bytes_remaining = dmc.sample_length;
        }
    }
}

static void dmc_step(void) {
    if (dmc.bits_remaining == 0) {
        dmc.bits_remaining = 8;
        if (dmc.buffer_empty) {
            // 버퍼가 비어있으면 출력 카운터 불변
            return;
        } else {
            dmc.shift_reg = dmc.sample_buffer;
            dmc.buffer_empty = 1;
            dmc_fetch_byte();
        }
    }

    // 1비트 델타 변조: 1이면 +2, 0이면 -2
    if (dmc.shift_reg & 1) {
        if (dmc.output_level <= 125) dmc.output_level += 2;
    } else {
        if (dmc.output_level >= 2)   dmc.output_level -= 2;
    }
    dmc.shift_reg >>= 1;
    dmc.bits_remaining--;
}
// ========================================================
// 고음질 & 고음량 오디오 프레임 합성
// ========================================================
static void generate_audio_frame(int16_t *out_samples) {
    // 60Hz 단위 길이 카운터 감쇠 (음표 꺼짐 처리)
    if (pulse1.length_counter > 0) pulse1.length_counter--;
    if (pulse2.length_counter > 0) pulse2.length_counter--;
    if (triangle.length_counter > 0) triangle.length_counter--;
    if (noise.length_counter > 0) noise.length_counter--;

    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
        double pulse_sample = 0.0;
        double tri_sample = 0.0;
        double noise_sample = 0.0;
        double dmc_sample   = 0.0;

        // 1. Pulse 1 (주파수 합성 및 DC 제거)
        if (pulse1.enabled && pulse1.timer_period >= 8 && pulse1.length_counter > 0) {
            double freq = 1789773.0 / (16.0 * (pulse1.timer_period + 1));
            if (freq >= 20.0 && freq <= 16000.0) {
                pulse1.phase += freq / AUDIO_SAMPLE_RATE;
                if (pulse1.phase >= 1.0) pulse1.phase -= 1.0;
                int step = (int)(pulse1.phase * 8.0) & 7;
                pulse_sample += duty_table[pulse1.duty][step] ? (double)pulse1.volume : -(double)pulse1.volume;
            }
        }

        // 2. Pulse 2
        if (pulse2.enabled && pulse2.timer_period >= 8 && pulse2.length_counter > 0) {
            double freq = 1789773.0 / (16.0 * (pulse2.timer_period + 1));
            if (freq >= 20.0 && freq <= 16000.0) {
                pulse2.phase += freq / AUDIO_SAMPLE_RATE;
                if (pulse2.phase >= 1.0) pulse2.phase -= 1.0;
                int step = (int)(pulse2.phase * 8.0) & 7;
                pulse_sample += duty_table[pulse2.duty][step] ? (double)pulse2.volume : -(double)pulse2.volume;
            }
        }

        // 3. Triangle
        if (triangle.enabled && triangle.timer_period >= 4 && triangle.length_counter > 0) {
            double freq = 1789773.0 / (32.0 * (triangle.timer_period + 1));
            if (freq >= 20.0 && freq <= 16000.0) {
                triangle.phase += freq / AUDIO_SAMPLE_RATE;
                if (triangle.phase >= 1.0) triangle.phase -= 1.0;
                int step = (int)(triangle.phase * 32.0) & 31;
                tri_sample = (double)(triangle_table[step] - 7.5); // 중심 0 정렬
            }
        }

        // 4. Noise (LFSR 시프트 레지스터 기반 모사)
        if (noise.enabled && noise.timer_period > 0 && noise.length_counter > 0) {
            double freq = 1789773.0 / (double)noise.timer_period;
            noise.phase += freq / AUDIO_SAMPLE_RATE;
            while (noise.phase >= 1.0) {
                noise.phase -= 1.0;
                uint16_t bit0 = noise.shift_reg & 1;
                uint16_t bit1 = (noise.shift_reg >> (noise.mode ? 6 : 1)) & 1;
                uint16_t feedback = bit0 ^ bit1;
                noise.shift_reg = (noise.shift_reg >> 1) | (feedback << 14);
            }
            if (!(noise.shift_reg & 1)) {
                noise_sample = (double)noise.volume;
            } else {
                noise_sample = -(double)noise.volume;
            }
        }

       // 5. DMC (DPCM 채널)
        if (dmc.enabled && dmc.rate_period > 0) {
            double freq = 1789773.0 / (double)dmc.rate_period;
            dmc.phase += freq / AUDIO_SAMPLE_RATE;
            while (dmc.phase >= 1.0) {
                dmc.phase -= 1.0;
                dmc_step();
            }
            // 0~127 레벨을 중심 0(-64 ~ +63)으로 오프셋 제거
            dmc_sample = (double)dmc.output_level - 64.0;
        }

       // ----------------------------------------------------
        // 5채널 믹서: 전 채널 밸런스 조정 및 리미터
        // ----------------------------------------------------
        double final_out = (pulse_sample * 800.0) +
                           (tri_sample   * 1000.0) +
                           (noise_sample * 700.0) +
                           (dmc_sample   * 180.0);

        if (final_out > 31000.0)  final_out = 31000.0;
        if (final_out < -31000.0) final_out = -31000.0;

        out_samples[i] = (int16_t)final_out;
    }
}

// ========================================================
// 에뮬레이터 코어 전역 상태 변수
// ========================================================
static uint8_t *rom, *chrrom,
    prg[4], chr[8],
    prgbits = 14, chrbits = 12,
    A, X, Y, P = 4, S = ~2, pc_h, pc_l,
    addr_lo, addr_hi,
    nomem, result, val, cross, tmp,
    ppumask, ppuctrl, ppustatus,
    ppubuf, W, fine_x, opcode,
    nmi_irq, ntb, ptb_lo,
    vram[2048], palette_ram[64], ram[8192],
    chrram[8192], prgram[8192], oam[256],
    mask[] = {128, 64, 1, 2, 1, 0, 0, 1, 4, 0, 0, 4, 0, 0, 64, 0, 8, 0, 0, 8},
    keys, mirror,
    mmc1_bits, mmc1_data, mmc1_ctrl,
    mmc3_chrprg[8], mmc3_bits,
    mmc3_irq, mmc3_latch,
    chrbank0, chrbank1, prgbank,
    rombuf[4*1024 * 1024];

static uint16_t scany, T, V, sum, dot, atb, shift_hi, shift_lo, cycles;
static int shift_at = 0;
static uint8_t vb_key_state = 0;
static uint32_t frame_buffer[256 * 240];

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

// Mapper 228 전용 $4020-$4023 4비트 RAM (4 nibbles)
static uint8_t m228_ram[4];

static uint8_t mmc3_bits = 0;
static uint8_t mmc3_chrprg[8];
static uint8_t mmc3_latch = 0;
static uint8_t mmc3_counter = 0;
static uint8_t mmc3_reload = 0;
static uint8_t mmc3_irq_enable = 0;
static uint8_t mmc3_a12_last = 0;
static int mmc3_a12_cycles = 0;

static uint8_t mmc1_count = 0;
static uint8_t mmc1_ctrl = 0x0C; // 기본 16KB 모드 (마지막 뱅크 고정)
static uint8_t mmc1_chr0 = 0;
static uint8_t mmc1_chr1 = 0;
static uint8_t mmc1_prg = 0;
static uint8_t mmc1_last_cycle = 0; // 연속 쓰기 방지 플래그

// FCEUX MMC1 하드웨어 상태 레지스터
static uint8_t mmc1_dregs[4];      // DRegs[0]: Control, [1]: CHR0, [2]: CHR1, [3]: PRG
static uint8_t mmc1_buffer = 0;    // Buffer
static uint8_t mmc1_shift = 0;     // BufferShift

static uint8_t m225_ram[4];

static uint32_t chr_mask = 0; // 전역 추가


static void MMC1_SyncMirror(void) {
    switch (mmc1_dregs[0] & 3) {
    case 0: mirror = 0; break; // One-Screen Lower ($2000)
    case 1: mirror = 1; break; // One-Screen Upper ($2400)
    case 2: mirror = 2; break; // Vertical
    case 3: mirror = 3; break; // Horizontal
    }
}

static void MMC1_SyncCHR(void) {
    if (rombuf[5] > 0) { // CHR-ROM 카트리지
        if (mmc1_dregs[0] & 0x10) {
            // 4KB 모드 2개
            chr[0] = mmc1_dregs[1];
            chr[1] = mmc1_dregs[2];
        } else {
            // 8KB 모드 (하위 비트 무시)
            chr[0] = mmc1_dregs[1] & 0xFE;
            chr[1] = mmc1_dregs[1] | 1;
        }
    } else { // CHR-RAM 카트리지 (Hillsfar 등)
        chr[0] = 0;
        chr[1] = 1;
    }
}

static void MMC1_SyncPRG(void) {
    // SUROM (512KB PRG) 오프셋: CHR0 레지스터의 비트 4
    uint8_t offs_16banks = (rombuf[4] >= 32) ? (mmc1_dregs[1] & 0x10) : 0;
    uint8_t prg_reg = mmc1_dregs[3] & 0x0F;

    switch (mmc1_dregs[0] & 0x0C) {
    case 0x0C: // Mode 3: $8000 스위칭, $C000 마지막 16KB 고정
        prg[0] = prg_reg + offs_16banks;
        prg[1] = 0x0F + offs_16banks;
        break;

    case 0x08: // Mode 2: $8000 첫 16KB 고정, $C000 스위칭
        prg[0] = offs_16banks;
        prg[1] = prg_reg + offs_16banks;
        break;

    case 0x00:
    case 0x04: // Mode 0, 1: 32KB 연속 스위칭
        prg[0] = (prg_reg & ~1) + offs_16banks;
        prg[1] = (prg_reg & ~1) + offs_16banks + 1;
        break;
    }
}

static void MMC1_SyncAll(void) {
    MMC1_SyncMirror();
    MMC1_SyncCHR();
    MMC1_SyncPRG();
}

static void mmc3_check_a12(uint16_t addr) {
    if (cur_mapper != 4) return;

    // A12 핀이 0 -> 1로 토글되는 순간 감지
    uint8_t a12 = (addr >> 12) & 1;
    if (a12 && !mmc3_a12_last) {
        // PPU 클럭 노이즈 방지 (충분한 주기 경과 확인)
        if (mmc3_counter == 0 || mmc3_reload) {
            mmc3_counter = mmc3_latch;
            mmc3_reload = 0;
        } else {
            mmc3_counter--;
        }

        if (mmc3_counter == 0 && mmc3_irq_enable) {
            nmi_irq |= 1; // IRQ 발생
        }
    }
    mmc3_a12_last = a12;
}

static uint8_t* get_chr_byte(uint16_t addr) {
    // 1. CHR-RAM 게임 (rombuf[5] == 0)은 8KB 단일 램 버퍼를 직접 반환
    if (rombuf[5] == 0) {
        return &chrram[addr & 0x1FFF];
    }

    // 2. CHR-ROM 게임은 기존 가변 뱅킹 수행
    uint32_t bank_size = 1 << chrbits;                 // 1KB 또는 4KB
    uint32_t slot = (addr & 0x1FFF) >> chrbits;
    uint32_t bank = chr[slot & 7];
    uint32_t offset = (bank << chrbits) | (addr & (bank_size - 1));

    if (chr_mask > 0) {
        offset &= chr_mask;
    }
    return &chrrom[offset];
}

static uint8_t* get_nametable_byte(uint16_t addr) {
    uint16_t nt = (addr - 0x2000) & 0x0FFF;
    uint16_t base = 0;

    switch (mirror) {
    case 0: // One-screen Lower ($2000)
        base = nt & 0x03FF;
        break;
    case 1: // One-screen Upper ($2400)
        base = 0x0400 | (nt & 0x03FF);
        break;
    case 2: // Vertical
        base = nt & 0x07FF;
        break;
    case 3: // Horizontal
        base = ((nt >> 1) & 0x0400) | (nt & 0x03FF);
        break;
    }
    return &vram[base & 0x07FF]; // 2KB VRAM 내로 안전하게 클램핑
}

 static uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write) {
    // 1. 순수 16비트 CPU 주소 보존
    uint16_t raw_addr = ((uint16_t)hi << 8) | lo;
    uint16_t addr = raw_addr;

    switch (hi >>= 4) {
    case 0: case 1:
        return write ? (ram[addr & 0x07FF] = val) : ram[addr & 0x07FF];

    case 2: case 3:
        lo &= 7;
        if (lo == 7) {
            tmp = ppubuf;
            uint8_t *r;

            if (V < 8192) {
                // CHR-RAM 게임(rombuf[5] == 0)인 경우 직접 chrram 버퍼에 쓰기 허용
                if (!rombuf[5]) {
                    r = &chrram[V & 0x1FFF];
                } else {
                    r = write ? &tmp : get_chr_byte(V);
                }
            } else if (V < 16128) {
                r = get_nametable_byte(V);
            } else {
                r = palette_ram + (uint8_t)((V & 19) == 16 ? V ^ 16 : V);
            }

            write ? (*r = val) : (ppubuf = *r);
            V += (ppuctrl & 4) ? 32 : 1;
            V %= 16384;
            return tmp;
        }

        if (write) {
            switch (lo) {
            case 0: ppuctrl = val; T = (T & 0xf3ff) | ((val % 4) << 10); break;
            case 1: ppumask = val; break;
            case 5: T = (W ^= 1) ? (fine_x = val & 7, (T & ~31) | (val / 8))
                                 : ((T & 0x8c1f) | ((val % 8) << 12) | ((val * 4) & 0x3e0)); break;
            case 6:
                if (W ^= 1) {
                    T = (T & 0x00FF) | ((uint16_t)(val & 0x3F) << 8);
                } else {
                    T = (T & 0xFF00) | val;
                    V = T; // T 전체를 V로 복사
                }
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
        if (lo >= 32) {
            // $4020 ~ $40FF: Mapper 228 RAM ($4020 ~ $4023 미러링)
            if (cur_mapper == 228) {
                uint8_t ram_idx = lo & 3;
                if (write) {
                    m228_ram[ram_idx] = val & 0x0F;
                    return 0;
                } else {
                    return m228_ram[ram_idx] & 0x0F;
                }
            }
            return 0;
        }

        // $4000 ~ $401F APU & 컨트롤러 레지스터 처리
        if (write) {
            if (lo == 20) {
                for (uint16_t i = 256; i--;) oam[i] = mem(i, val, 0, 0);
            }
            switch (lo) {
            case 0: pulse1.duty = (val >> 6) & 3; pulse1.volume = val & 15; break;
            case 2: pulse1.timer_period = (pulse1.timer_period & 0x0700) | val; break;
            case 3:
                pulse1.timer_period = (pulse1.timer_period & 0x00FF) | ((val & 7) << 8);
                pulse1.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                pulse1.phase = 0;
                break;
            case 4: pulse2.duty = (val >> 6) & 3; pulse2.volume = val & 15; break;
            case 6: pulse2.timer_period = (pulse2.timer_period & 0x0700) | val; break;
            case 7:
                pulse2.timer_period = (pulse2.timer_period & 0x00FF) | ((val & 7) << 8);
                pulse2.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                pulse2.phase = 0;
                break;
            case 8: triangle.linear_counter = val & 0x7F; break;
            case 10: triangle.timer_period = (triangle.timer_period & 0x0700) | val; break;
            case 11:
                triangle.timer_period = (triangle.timer_period & 0x00FF) | ((val & 7) << 8);
                triangle.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                triangle.phase = 0;
                break;
            case 12: noise.volume = val & 15; break;
            case 14:
                noise.mode = (val >> 7) & 1;
                noise.timer_period = noise_period_table[val & 15];
                break;
            case 15:
                noise.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                break;
            case 16:
                dmc.irq_enabled = (val >> 7) & 1;
                dmc.loop = (val >> 6) & 1;
                dmc.rate_period = dmc_period_table[val & 15];
                break;
            case 17:
                dmc.output_level = val & 0x7F;
                break;
            case 18:
                dmc.sample_address = 0xC000 | ((uint16_t)val << 6);
                break;
            case 19:
                dmc.sample_length = ((uint16_t)val << 4) | 1;
                break;
            case 21:
                pulse1.enabled   = val & 1;
                pulse2.enabled   = (val >> 1) & 1;
                triangle.enabled = (val >> 2) & 1;
                noise.enabled    = (val >> 3) & 1;
                dmc.enabled      = (val >> 4) & 1;

                if (!pulse1.enabled)   pulse1.length_counter = 0;
                if (!pulse2.enabled)   pulse2.length_counter = 0;
                if (!triangle.enabled) triangle.length_counter = 0;
                if (!noise.enabled)    noise.length_counter = 0;

                if (dmc.enabled) {
                    if (dmc.bytes_remaining == 0) {
                        dmc.current_address = dmc.sample_address;
                        dmc.bytes_remaining = dmc.sample_length;
                        dmc_fetch_byte();
                    }
                } else {
                    dmc.bytes_remaining = 0;
                }
                break;
            }
        }

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

    case 5: // $5000 ~ $5FFF
        if (cur_mapper == 228) {
            uint8_t ram_idx = lo & 3;
            if (write) {
                m228_ram[ram_idx] = val & 0x0F;
                return 0;
            } else {
                return m228_ram[ram_idx] & 0x0F;
            }
        }

        // Mapper 225 / 255 보안 RAM ($5800 ~ $5FFF, raw_addr & 0x800)
        if (cur_mapper == 225 || cur_mapper == 255) {
            if (raw_addr & 0x0800) {
                uint8_t ram_idx = raw_addr & 3;
                if (write) {
                    m225_ram[ram_idx] = val & 0x0F;
                    return 0;
                } else {
                    return m225_ram[ram_idx] & 0x0F;
                }
            }
          return 0;
         }
        return 0;
    case 6: case 7:
       // addr = raw_addr & 8191;
       // return write ? (prgram[addr] = val) : prgram[addr];
       addr = raw_addr & 0x1FFF; // 8KB PRG-RAM ($6000 ~ $7FFF)
        return write ? (prgram[addr] = val) : prgram[addr];

    default: // $8000 ~ $FFFF
        if (write) {
            // ========================================================
            // 1. Mapper 228 (Action 52 / Cheetahmen II)
            // ========================================================
            if (cur_mapper == 228) {
                mirror = (raw_addr & 0x2000) ? 3 : 2;

                uint32_t chip     = (raw_addr >> 11) & 0x03;
                uint32_t prg_sub  = (raw_addr >> 6)  & 0x1F;
                uint32_t prg_bank = (chip << 5) | prg_sub;

                if (raw_addr & 0x0020) {
                    prg[0] = prg_bank;
                    prg[1] = prg_bank;
                } else {
                    prg[0] = prg_bank & ~1;
                    prg[1] = prg_bank | 1;
                }

                uint32_t chr_bank = ((raw_addr & 0x000F) << 2) | (val & 0x03);
                uint32_t chr_total = (rombuf[5] > 0) ? ((uint32_t)rombuf[5] * 2) : 2;
                chr[0] = (chr_bank * 2)     % chr_total;
                chr[1] = (chr_bank * 2 + 1) % chr_total;
                return 0;
            }

             // ========================================================
            // Mapper 15 (100-in-1 Contra Function 16 / FCEUX 정밀 구현)
            // ========================================================
            if (cur_mapper == 15) {
                // 1. 미러링: ((latched >> 6) & 1) ^ 1
                // 0이면 Vertical(2), 1이면 Horizontal(3)
                mirror = (((val >> 6) & 1) ^ 1) ? 3 : 2;

                uint8_t mode = raw_addr & 3;
                uint8_t base_bank = val & 0x3F;

                switch (mode) {
                case 0:
                    // 32KB 순차 매핑 (8KB 슬롯 4개)
                    for (int i = 0; i < 4; i++) {
                        prg[i] = (base_bank << 1) + i;
                    }
                    break;

                case 2:
                    // 8KB 단위 동일 매핑 ((val & 0x3F) << 1 + (val >> 7))
                    for (int i = 0; i < 4; i++) {
                        prg[i] = (base_bank << 1) + (val >> 7);
                    }
                    break;

                case 1:
                case 3:
                    // Mode 1: 128KB 블록 내 16KB 스위칭 (i >= 2일 때 bank | 7)
                    // Mode 3: 16KB 양쪽 동일 복제 매핑 (bank 그대로 복제)
                    for (int i = 0; i < 4; i++) {
                        uint8_t b = base_bank;
                        if (i >= 2 && !(mode & 2)) {
                            b |= 0x07;
                        }
                        prg[i] = (b << 1) + (i & 1);
                    }
                    break;
                }
                return 0;
            }
           
            // ========================================================
            // 2. Mapper 4 (MMC3 - Dragon Strike 등) 정식 8개 레지스터
            // ========================================================
            if (cur_mapper == 4) {
                uint16_t reg = raw_addr & 0xE001;
                switch (reg) {
                case 0x8000:
                    mmc3_bits = val;
                    break;
                case 0x8001: {
                    mmc3_chrprg[mmc3_bits & 7] = val;
                    int chr_invert = (mmc3_bits >> 7) & 1;
                    int prg_mode   = (mmc3_bits >> 6) & 1;

                    uint8_t r0 = mmc3_chrprg[0] & ~1;
                    uint8_t r1 = mmc3_chrprg[1] & ~1;
                    uint8_t r2 = mmc3_chrprg[2];
                    uint8_t r3 = mmc3_chrprg[3];
                    uint8_t r4 = mmc3_chrprg[4];
                    uint8_t r5 = mmc3_chrprg[5];

                    if (!chr_invert) {
                        chr[0] = r0;     chr[1] = r0 + 1;
                        chr[2] = r1;     chr[3] = r1 + 1;
                        chr[4] = r2;     chr[5] = r3;
                        chr[6] = r4;     chr[7] = r5;
                    } else {
                        chr[0] = r2;     chr[1] = r3;
                        chr[2] = r4;     chr[3] = r5;
                        chr[4] = r0;     chr[5] = r0 + 1;
                        chr[6] = r1;     chr[7] = r1 + 1;
                    }

                    int prg_last = (rombuf[4] * 2) - 1;
                    if (!prg_mode) {
                        prg[0] = mmc3_chrprg[6];
                        prg[1] = mmc3_chrprg[7];
                        prg[2] = prg_last - 1;
                        prg[3] = prg_last;
                    } else {
                        prg[0] = prg_last - 1;
                        prg[1] = mmc3_chrprg[7];
                        prg[2] = mmc3_chrprg[6];
                        prg[3] = prg_last;
                    }
                    break;
                }
                case 0xA000:
                    mirror = (val & 1) ? 3 : 2;
                    break;
                case 0xA001:
                    break;
                case 0xC000:
                    mmc3_latch = val;
                    break;
                case 0xC001:
                    mmc3_reload = 1;
                    break;
                case 0xE000:
                    mmc3_irq_enable = 0;
                    nmi_irq &= ~1;
                    break;
                case 0xE001:
                    mmc3_irq_enable = 1;
                    break;
                }
                return 0;
            }

         
           // ========================================================
            // Mapper 1 (MMC1 / FCEUX 정밀 구현)
            // ========================================================
            if (cur_mapper == 1) {
                int n = (raw_addr >> 13) - 4; // 0x8000->0, 0xA000->1, 0xC000->2, 0xE000->3

                if (val & 0x80) {
                    mmc1_dregs[0] |= 0x0C;
                    mmc1_buffer = 0;
                    mmc1_shift = 0;
                    MMC1_SyncPRG();
                    return 0;
                }

                mmc1_buffer |= (val & 1) << (mmc1_shift++);

                if (mmc1_shift == 5) {
                    mmc1_dregs[n] = mmc1_buffer;
                    mmc1_buffer = 0;
                    mmc1_shift = 0;

                    switch (n) {
                    case 0:
                        MMC1_SyncMirror();
                        MMC1_SyncCHR();
                        MMC1_SyncPRG();
                        break;
                    case 1:
                        MMC1_SyncCHR();
                        MMC1_SyncPRG(); // DRegs[1]은 SUROM PRG 뱅크 오프셋을 변경하므로 PRG도 동기화
                        break;
                    case 2:
                        MMC1_SyncCHR();
                        break;
                    case 3:
                        MMC1_SyncPRG();
                        break;
                    }
                }
                return 0;
            }
           
           // ========================================================
            // Mapper 225 / 255 (72-in-1, 115-in-1 등 멀티 합팩)
            // ========================================================
            if (cur_mapper == 225 || cur_mapper == 255) {
                uint8_t bank = (raw_addr >> 14) & 1;
                uint8_t mirr = (raw_addr >> 13) & 1;
                uint8_t mode = (raw_addr >> 12) & 1;
                uint8_t chr_bank = (raw_addr & 0x3F) | (bank << 6);
                uint8_t prg_bank = ((raw_addr >> 6) & 0x3F) | (bank << 6);

                // 미러링: mirr ^ 1 (0이면 Vertical(2), 1이면 Horizontal(3))
                mirror = (mirr ^ 1) ? 3 : 2;

                // PRG 뱅킹 (mode: 1 = 16KB 동일 매핑, 0 = 32KB 연속 매핑)
                if (mode) {
                    prg[0] = prg_bank;
                    prg[1] = prg_bank;
                } else {
                    prg[0] = prg_bank & ~1;
                    prg[1] = (prg_bank & ~1) | 1;
                }

                // CHR 8KB 뱅킹 (4KB 슬롯 2개 기준)
                if (rombuf[5] > 0) {
                    uint32_t chr_total = (uint32_t)rombuf[5] * 2;
                    chr[0] = (chr_bank * 2)     % chr_total;
                    chr[1] = (chr_bank * 2 + 1) % chr_total;
                } else {
                    chr[0] = 0;
                    chr[1] = 1;
                }
                return 0;
            }
            // ========================================================
            // 4. 기타 기본 매퍼 (AxROM, CNROM, UxROM)
            // ========================================================
            switch (rombuf[6] >> 4) {
            case 7:
                mirror = !(val / 16);
                prg[0] = val % 8 * 2;
                prg[1] = prg[0] + 1;
                break;
            case 3:
                chr[0] = val % 4 * 2;
                chr[1] = chr[0] + 1;
                break;
            case 2:
                prg[0] = val & 31;
                break;
            }
        }

        // ========================================================
        // ROM 읽기: prgbits (8KB: 13 / 16KB: 14) 단위 가변 인덱싱
        // ========================================================
        uint32_t bank_size = 1 << prgbits;                  // 8192 또는 16384
        uint32_t slot = (raw_addr - 0x8000) >> prgbits;     // 8KB: 0~3, 16KB: 0~1
        uint32_t bank_idx = prg[slot & 3];

        // 16KB 단위인 경우 rombuf[4], 8KB 단위인 경우 rombuf[4] * 2
        uint32_t max_banks = (prgbits == 13) ? ((uint32_t)rombuf[4] * 2) : (uint32_t)rombuf[4];
        if (max_banks > 0) {
            bank_idx %= max_banks;
        }

        uint32_t rom_addr = (bank_idx << prgbits) | (raw_addr & (bank_size - 1));
        
        // 카트리지 PRG ROM 총 바이트 크기 초과 방지 마스크
        uint32_t prg_total_bytes = (uint32_t)rombuf[4] * 16384;
        if (prg_total_bytes > 0) {
            rom_addr %= prg_total_bytes;
        }

        return rom[rom_addr];
    }
    return ~0;
}

static uint8_t read_pc() {
    val = mem(pc_l, pc_h, 0, 0);
    !++pc_l && ++pc_h;
    return val;
}

static uint8_t set_nz(uint8_t val) {
    return P = P & 125 | val & 128 | !val * 2;
}

// ========================================================
// Edge2X (Scale2x) 512x480 스케일러 버퍼 및 함수
// ========================================================
static uint32_t frame_buffer[256 * 240];        // 원본 PPU 256x240 버퍼
static uint32_t scaled_buffer[512 * 480];       // Edge2x 512x480 출력 버퍼

static void apply_edge2x(void) {
    const int W = 256;
    const int H = 240;

    for (int y = 0; y < H; y++) {
        int ym1 = (y > 0) ? (y - 1) : 0;
        int yp1 = (y < H - 1) ? (y + 1) : (H - 1);

        const uint32_t *row_mid = &frame_buffer[y * W];
        const uint32_t *row_top = &frame_buffer[ym1 * W];
        const uint32_t *row_bot = &frame_buffer[yp1 * W];

        uint32_t *out_row0 = &scaled_buffer[(y * 2) * (W * 2)];
        uint32_t *out_row1 = &scaled_buffer[(y * 2 + 1) * (W * 2)];

        for (int x = 0; x < W; x++) {
            int xm1 = (x > 0) ? (x - 1) : 0;
            int xp1 = (x < W - 1) ? (x + 1) : (W - 1);

            uint32_t E = row_mid[x];
            uint32_t B = row_top[x];
            uint32_t D = row_mid[xm1];
            uint32_t F = row_mid[xp1];
            uint32_t H_val = row_bot[x];

            uint32_t e0 = E, e1 = E, e2 = E, e3 = E;

            // 모서리(Edge) 대각선 보간 조건
            if (B != H_val && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H_val) e2 = D;
                if (H_val == F) e3 = F;
            }

            int out_x = x * 2;
            out_row0[out_x]     = e0;
            out_row0[out_x + 1] = e1;
            out_row1[out_x]     = e2;
            out_row1[out_x + 1] = e3;
        }
    }
}

// 기존 NES_GetBuffer 함수가 512x480 버퍼 포인터를 반환하도록 변경
__declspec(dllexport) uint32_t* __stdcall NES_GetBuffer(void) {
    return scaled_buffer;
}

// ========================================================
// DLL 익스포트 함수
// ========================================================


__declspec(dllexport) int __stdcall NES_LoadROM(const char* filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size > (long)sizeof(rombuf)) {
        fclose(fp);
        return 0; // 버퍼 초과 방지
    }

    memset(rombuf, 0, sizeof(rombuf));
    size_t read_bytes = fread(rombuf, 1, sizeof(rombuf), fp);
    fclose(fp);

    if (read_bytes < 16 || memcmp(rombuf, "NES\x1A", 4) != 0) return 0;

    // iNES 매퍼 번호 계산
    cur_mapper = ((rombuf[7] & 0xF0)) | (rombuf[6] >> 4);

    // ========================================================
    // 1. 하드웨어 전역 상태 완벽 초기화
    // ========================================================
    memset(ram, 0, sizeof(ram));
    memset(vram, 0, sizeof(vram));
    memset(palette_ram, 0, sizeof(palette_ram));
    memset(oam, 0, sizeof(oam));
    memset(prgram, 0, sizeof(prgram));
    memset(frame_buffer, 0, sizeof(frame_buffer));
    memset(chrram, 0, sizeof(chrram));

    // 매퍼 내부 변수 초기화
    mmc1_bits = 5;
    mmc1_data = 0;
    mmc1_ctrl = 0x0C;
    chrbank0 = chrbank1 = prgbank = 0;

    memset(mmc3_chrprg, 0, sizeof(mmc3_chrprg));
    mmc3_bits = 0;
    mmc3_latch = 0;
    mmc3_counter = 0;
    mmc3_reload = 0;
    mmc3_irq_enable = 0;

    // PPU 스크롤/상태 레지스터 초기화
    T = V = 0;
    fine_x = 0;
    W = 0;
    ppuctrl = 0;
    ppumask = 0;
    ppustatus = 0;
    ppubuf = 0;
    ntb = ptb_lo = 0;
    shift_hi = shift_lo = shift_at = atb = 0;

    // CPU 레지스터 초기화
    A = X = Y = 0;
    P = 4;
    S = ~2; // 스택 포인터 0xFD
    dot = scany = nmi_irq = 0;

    // ========================================================
    // 2. ROM / CHR 포인터 세팅 (Trainer 플래그 지원)
    // ========================================================
    int trainer_offset = (rombuf[6] & 4) ? 512 : 0;
    rom = rombuf + 16 + trainer_offset;

    uint32_t prg_total_bytes = (uint32_t)rombuf[4] * 16384;
    chrrom = rombuf[5] ? (rom + prg_total_bytes) : chrram;

    // ========================================================
    // 2. ROM / CHR 포인터 세팅
    // ========================================================
    /*
    rom = rombuf + 16;
    uint32_t prg_total_bytes = (uint32_t)rombuf[4] * 16384;
    chrrom = rombuf[5] ? (rom + prg_total_bytes) : chrram; */

    if (rombuf[5] > 0) {
        chr_mask = ((uint32_t)rombuf[5] * 8192) - 1;
    } else {
        chr_mask = 8191;
    }

    // 기본 미러링 (iNES 헤더 Bit 0: 0=Horizontal(3), 1=Vertical(2))
    mirror = (rombuf[6] & 1) ? 2 : 3;

    // ========================================================
    // 3. 매퍼별 초기 뱅크 설정 (리셋 벡터 읽기 전에 반드시 완료)
    // ========================================================
    if (cur_mapper == 228) {
        memset(m228_ram, 0, sizeof(m228_ram));
        prgbits = 14;   // 16KB 단위
        chrbits = 12;   // 4KB 단위
        prg[0] = 0;
        prg[1] = 1;
        chr[0] = 0;
        chr[1] = 1;
        mirror = 2;     // Vertical 기본
    } 
    else if (cur_mapper == 4) { // MMC3
        prgbits = 13;   // [필수] MMC3 PRG는 8KB 단위!
        chrbits = 10;   // [필수] MMC3 CHR는 1KB 단위!

        uint32_t prg_last = (rombuf[4] * 2) - 1; // 8KB 뱅크 총 개수 - 1
        prg[0] = 0;
        prg[1] = 1;
        prg[2] = prg_last - 1; // 끝에서 두 번째 8KB ($C000~$DFFF 고정)
        prg[3] = prg_last;     // 맨 마지막 8KB ($E000~$FFFF 고정, 리셋 벡터 위치)

        chr[0] = 0; chr[1] = 1;
        chr[2] = 2; chr[3] = 3;
        chr[4] = 4; chr[5] = 5;
        chr[6] = 6; chr[7] = 7;
    } 
   /*
   else if (cur_mapper == 1) { // MMC1
        prgbits = 14;   // 16KB 단위
        chrbits = 12;   // 4KB 단위

        mmc1_shift = 0;
        mmc1_count = 0;
        mmc1_ctrl = 0x0C; // PRG 모드 3 ($C000 마지막 뱅크 고정)
        mmc1_chr0 = 0;
        mmc1_chr1 = 0;
        mmc1_prg = 0;

        uint32_t last_16k = rombuf[4] ? (rombuf[4] - 1) : 0;
        prg[0] = 0;
        prg[1] = last_16k; // 리셋 벡터가 있는 카트리지 마지막 16KB 고정
        chr[0] = 0;
        chr[1] = 1;
        mirror = 0;
    } */
   else if (cur_mapper == 1) { // MMC1
        prgbits = 14;   // 16KB 단위
        chrbits = 12;   // 4KB 단위

        for (int i = 0; i < 4; i++) mmc1_dregs[i] = 0;
        mmc1_buffer = 0;
        mmc1_shift = 0;
        mmc1_dregs[0] = 0x1F; // FCEUX Reset 표준값 (0x1F)

        MMC1_SyncAll();
    }
   else if (cur_mapper == 15) {
        prgbits = 13;   // 8KB 단위 (슬롯 4개: prg[0] ~ prg[3])
        chrbits = 12;   // CHR-RAM 8KB

        // latchea = 0x8000, latched = 0 상태의 초기 Sync() 결과
        prg[0] = 0;
        prg[1] = 1;
        prg[2] = 2;
        prg[3] = 3;

        chr[0] = 0;
        chr[1] = 1;
        mirror = 3;     // ((0 >> 6) & 1) ^ 1 = 1 -> Horizontal(3)
    }
    else if (cur_mapper == 225 || cur_mapper == 255) {
        prgbits = 14;   // 16KB 단위
        chrbits = 12;   // 4KB 단위 (8KB = 4KB 슬롯 2개)

        memset(m225_ram, 0, sizeof(m225_ram));

        // M225Power: prg = 0, mode = 0 (32KB 모드)
        prg[0] = 0;
        prg[1] = 1;
        chr[0] = 0;
        chr[1] = 1;
        mirror = 3;     // mirr = 0 -> mirr ^ 1 = 1 -> Horizontal(3)
    }
    else {
        // 기본 Mapper 0 / 2 / 3
        prgbits = 14; // 16KB 단위
        chrbits = 12; // 4KB 단위
        prg[0] = 0;
        prg[1] = rombuf[4] ? (rombuf[4] - 1) : 0;
        chr[0] = 0;
        chr[1] = rombuf[5] ? (rombuf[5] * 2 - 1) : 1;
    }

    // ========================================================
    // 4. [핵심] MMC3 8KB 정렬 상태에서 CPU 리셋 벡터 읽기
    // ========================================================
    pc_l = mem(~3, ~0, 0, 0); // $FFFD
    pc_h = mem(~2, ~0, 0, 0); // $FFFC

    // ========================================================
    // 5. APU 오디오 상태 초기화
    // ========================================================
    memset(&pulse1, 0, sizeof(pulse1));
    memset(&pulse2, 0, sizeof(pulse2));
    memset(&triangle, 0, sizeof(triangle));
    memset(&noise, 0, sizeof(noise));
    noise.shift_reg = 1;

    memset(&dmc, 0, sizeof(dmc));
    dmc.output_level = 64;
    dmc.buffer_empty = 1;
    dmc.rate_period = dmc_period_table[0];

    return 1;
}

__declspec(dllexport) void __stdcall NES_SetInput(uint8_t buttons) {
    vb_key_state = buttons;
}


__declspec(dllexport) int __stdcall NES_InitAudio(void) {
    if (!h_winmm_mod) {
        h_winmm_mod = LoadLibraryA("winmm.dll");
        if (!h_winmm_mod) return 0;
        fn_waveOutOpen = (pfn_waveOutOpen)GetProcAddress(h_winmm_mod, "waveOutOpen");
        fn_waveOutPrepareHeader = (pfn_waveOutPrepareHeader)GetProcAddress(h_winmm_mod, "waveOutPrepareHeader");
        fn_waveOutUnprepareHeader = (pfn_waveOutUnprepareHeader)GetProcAddress(h_winmm_mod, "waveOutUnprepareHeader");
        fn_waveOutWrite = (pfn_waveOutWrite)GetProcAddress(h_winmm_mod, "waveOutWrite");
        fn_waveOutReset = (pfn_waveOutReset)GetProcAddress(h_winmm_mod, "waveOutReset");
        fn_waveOutClose = (pfn_waveOutClose)GetProcAddress(h_winmm_mod, "waveOutClose");
        if (!fn_waveOutOpen || !fn_waveOutWrite) return 0;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

   // if (fn_waveOutOpen(&h_wave_out, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) {
if (fn_waveOutOpen((void **)&h_wave_out, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) {
        return 0;
    }

    audio_buf_idx = 0;
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        memset(&wave_hdrs[i], 0, sizeof(WAVEHDR));
        memset(audio_buffers[i], 0, sizeof(audio_buffers[i]));
        wave_hdrs[i].lpData = (char*)audio_buffers[i];
        wave_hdrs[i].dwBufferLength = SAMPLES_PER_FRAME * sizeof(int16_t);
        wave_hdrs[i].dwFlags = WHDR_DONE; // 최초에 전송 가능하도록 DONE 마킹
        fn_waveOutPrepareHeader(h_wave_out, &wave_hdrs[i], sizeof(WAVEHDR));
    }
    return 1;
}

__declspec(dllexport) void __stdcall NES_CloseAudio(void) {
    if (h_wave_out) {
        if (fn_waveOutReset) fn_waveOutReset(h_wave_out);
        for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
            if (fn_waveOutUnprepareHeader) fn_waveOutUnprepareHeader(h_wave_out, &wave_hdrs[i], sizeof(WAVEHDR));
        }
        if (fn_waveOutClose) fn_waveOutClose(h_wave_out);
        h_wave_out = NULL;
    }
    if (h_winmm_mod) {
        FreeLibrary(h_winmm_mod);
        h_winmm_mod = NULL;
    }
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
                !++pc_l && ++pc_h;
            nmi_irq_entry:
                PUSH(pc_h); PUSH(pc_l); PUSH(P | 32);
                uint16_t veclo = ~1 - (nmi_irq & 4);
                pc_l = mem(veclo, ~0, 0, 0);
                pc_h = mem(veclo + 1, ~0, 0, 0);
                nmi_irq = 0;
                cycles++;
                break;
            }
            case 1: result = read_pc(); PUSH(pc_h); PUSH(pc_l); pc_h = read_pc(); pc_l = result; break;
            case 2: P = PULL & ~32; pc_l = PULL; pc_h = PULL; break;
            case 3: pc_l = PULL; pc_h = PULL; !++pc_l && ++pc_h; break;
            }
            cycles += 4;
            break;

        case 16:
            read_pc();
            if (!(P & mask[opcode >> 6]) ^ opcode / 32 & 1) {
                cross = pc_l + (int8_t)val >> 8;
                pc_h += cross;
                pc_l += val;
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
            read_pc(); val += X;
            addr_lo = mem(val, 0, 0, 0); addr_hi = mem(val + 1, 0, 0, 0);
            cycles += 4;
            goto opcode_entry;

        case 2: case 9:
            read_pc(); nomem = 1; goto nomemop_entry;

        case 17:
            addr_lo = mem(read_pc(), 0, 0, 0); addr_hi = mem(val + 1, 0, 0, 0);
            cycles++;
            goto add_x_or_y_entry;

        case 4: case 5: case 6:
        case 20: case 21: case 22:
            addr_lo = read_pc(); cross = opcodelo5 > 6;
            if (cross) addr_lo += (opcode & 214) == 150 ? Y : X;
            addr_hi = 0; cycles -= !cross;
            goto opcode_entry;

        case 12: case 13: case 14:
        case 25:
        case 28: case 29: case 30:
            addr_lo = read_pc(); addr_hi = read_pc();
            if (opcodelo5 < 25) goto opcode_entry;
        add_x_or_y_entry:
            val = opcodelo5 < 28 | opcode == 190 ? Y : X;
            cross = addr_lo + val > 255;
            addr_hi += cross; addr_lo += val;
            cycles += ((opcode & 224) == 128 | opcode % 16 == 14 & opcode != 190) | cross;
        opcode_entry:
            cycles += 2;
            if (opcode != 76 & (opcode & 224) != 128) val = mem(addr_lo, addr_hi, 0, 0);

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
                result |= val * 2; P = P & ~1 | val / 128; goto memop_entry;
            case 98: result = P << 7;
            case 66:
                result |= val / 2; P = P & ~1 | val & 1; goto memop_entry;
            case 194: result = val - 1; goto memop_entry;
            case 226: result = val + 1;
            memop_entry:
                set_nz(result);
                nomem ? A = result : (cycles += 2, mem(addr_lo, addr_hi, result, 1));
                break;
            case 32: P = P & 61 | val & 192 | !(A & val) * 2; break;
            case 64: pc_l = addr_lo; pc_h = addr_hi; cycles--; break;
            case 96: pc_l = val; pc_h = mem(addr_lo + 1, addr_hi, 0, 0); cycles++; break;
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

   
      // PPU 클럭 처리
        for (tmp = cycles * 3 + 6; tmp--;) {
            if (ppumask & 24) {
                if (scany < 240) {
                    if (dot - 256 > 63u) {
                        if (dot < 256) {
                            uint8_t color = shift_hi >> 14 - fine_x & 2 | shift_lo >> 15 - fine_x & 1,
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
                                                 sprite_addr = (ppuctrl & 32 ? sprite_tile % 2 << 12 | sprite_tile << 4 & -32 | sy * 2 & 16
                                                                             : (ppuctrl & 8) << 9 | sprite_tile << 4) | sy & 7,
                                                 sprite_color = *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 | *get_chr_byte(sprite_addr) >> sx & 1;
                                        if (sprite_color) {
                                            if (!(sprite[2] & 32 && color)) {
                                                color = sprite_color;
                                                palette = 16 | sprite[2] * 4 & 12;
                                            }
                                            if (sprite == oam && color) ppustatus |= 64;
                                            break;
                                        }
                                    }
                                }
                            }
                            frame_buffer[scany * 256 + dot] = nes_palette32[palette_ram[color ? palette | color : 0] & 0x3F];
                        }

                        if (dot < 336) { shift_hi *= 2; shift_lo *= 2; shift_at *= 4; }

                        int temp = ppuctrl << 8 & 4096 | ntb << 4 | V >> 12;
                        switch (dot & 7) {
                        case 1: ntb = *get_nametable_byte(V); break;
                        case 3: atb = (*get_nametable_byte(V & 0xc00 | 0x3c0 | V >> 4 & 0x38 | V / 4 & 7) >> (V >> 5 & 2 | V / 2 & 1) * 2) % 4 * 0x5555; break;
                        case 5: ptb_lo = *get_chr_byte(temp); break; // mmc3_check_a12 제거 (중복 방지)
                        case 7: {
                            uint8_t ptb_hi = *get_chr_byte(temp | 8);
                            V = V % 32 == 31 ? V & ~31 ^ 1024 : V + 1;
                            shift_hi |= ptb_hi; shift_lo |= ptb_lo; shift_at |= atb;
                            break;
                        }
                        }
                    }

                    if (dot == 256) {
                        V = ((V & 7 << 12) != 7 << 12 ? V + 4096
                             : (V & 0x3e0) == 928     ? V & 0x8c1f ^ 2048
                             : (V & 0x3e0) == 0x3e0   ? V & 0x8c1f
                                                      : V & 0x8c1f | V + 32 & 0x3e0) & ~0x41f | T & 0x41f;
                    }
                }

                // ========================================================
                // MMC3 Scanline IRQ 타이머 클럭 (스캔라인당 1회: dot == 260)
                // ========================================================
                if (cur_mapper == 4 && scany < 240 && dot == 260) {
                    if (mmc3_counter == 0 || mmc3_reload) {
                        mmc3_counter = mmc3_latch;
                        mmc3_reload = 0;
                    } else {
                        mmc3_counter--;
                    }

                    if (mmc3_counter == 0 && mmc3_irq_enable) {
                        nmi_irq |= 1; // 6502 CPU IRQ 비트 플래그 세팅
                    }
                }

                if (scany == 261 && dot - 280 < 25u) V = V & 0x841f | T & 0x7be0;
            }

            if (dot == 1) {
                if (scany == 241) {
                    if (ppuctrl & 128) nmi_irq |= 4; // VBlank NMI
                    ppustatus |= 128;
                    frame_done = 1;
                }
                if (scany == 261) ppustatus = 0;
            }

            if (++dot == 341) { dot = 0; scany++; scany %= 262; }
        }
    }
        // PPU 클럭 처리
/*
        for (tmp = cycles * 3 + 6; tmp--;) {
            if (ppumask & 24) {
                if (scany < 240) {
                    if (dot - 256 > 63u) {
                        if (dot < 256) {
                            uint8_t color = shift_hi >> 14 - fine_x & 2 | shift_lo >> 15 - fine_x & 1,
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
                                                 sprite_addr = (ppuctrl & 32 ? sprite_tile % 2 << 12 | sprite_tile << 4 & -32 | sy * 2 & 16
                                                                             : (ppuctrl & 8) << 9 | sprite_tile << 4) | sy & 7,
                                                 sprite_color = *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 | *get_chr_byte(sprite_addr) >> sx & 1;
                                        if (sprite_color) {
                                            if (!(sprite[2] & 32 && color)) {
                                                color = sprite_color;
                                                palette = 16 | sprite[2] * 4 & 12;
                                            }
                                            if (sprite == oam && color) ppustatus |= 64;
                                            break;
                                        }
                                    }
                                }
                            }
                            frame_buffer[scany * 256 + dot] = nes_palette32[palette_ram[color ? palette | color : 0] & 0x3F];
                        }

                        if (dot < 336) { shift_hi *= 2; shift_lo *= 2; shift_at *= 4; }

                        int temp = ppuctrl << 8 & 4096 | ntb << 4 | V >> 12;
                        switch (dot & 7) {
                        case 1: ntb = *get_nametable_byte(V); break;
                        case 3: atb = (*get_nametable_byte(V & 0xc00 | 0x3c0 | V >> 4 & 0x38 | V / 4 & 7) >> (V >> 5 & 2 | V / 2 & 1) * 2) % 4 * 0x5555; break;
                        case 5: ptb_lo = *get_chr_byte(temp); mmc3_check_a12(temp);break;
                        case 7: {
                            uint8_t ptb_hi = *get_chr_byte(temp | 8);
                            V = V % 32 == 31 ? V & ~31 ^ 1024 : V + 1;
                            shift_hi |= ptb_hi; shift_lo |= ptb_lo; shift_at |= atb;
                            break;
                        }
                        }
                    }

                    if (dot == 256) {
                        V = ((V & 7 << 12) != 7 << 12 ? V + 4096
                             : (V & 0x3e0) == 928     ? V & 0x8c1f ^ 2048
                             : (V & 0x3e0) == 0x3e0   ? V & 0x8c1f
                                                      : V & 0x8c1f | V + 32 & 0x3e0) & ~0x41f | T & 0x41f;
                    }
                }

                // ========================================================
                // MMC3 Scanline IRQ 타이머 클럭 (dot == 260)
                // ========================================================
                if (cur_mapper == 4 && scany < 240 && dot == 260) {
                    if (mmc3_counter == 0 || mmc3_reload) {
                        mmc3_counter = mmc3_latch;
                        mmc3_reload = 0;
                    } else {
                        mmc3_counter--;
                    }

                    if (mmc3_counter == 0 && mmc3_irq_enable) {
                        nmi_irq |= 1; // IRQ 신호 발생 (CPU 코어 인터럽트 비트)
                    }
                }

                if (scany == 261 && dot - 280 < 25u) V = V & 0x841f | T & 0x7be0;
            }

            if (dot == 1) {
                if (scany == 241) {
                    if (ppuctrl & 128) nmi_irq |= 4; // VBlank NMI (누적 비트 플래그)
                    ppustatus |= 128;
                    frame_done = 1;
                }
                if (scany == 261) ppustatus = 0;
            }

            if (++dot == 341) { dot = 0; scany++; scany %= 262; }
        }
   }
*/
    // 1프레임 렌더링 완료 후 Edge2x 적용
    apply_edge2x();

    // ========================================================
    // 4중 버퍼 스트리밍 (오디오 언더런 완벽 방지)
    // ========================================================
    if (h_wave_out && fn_waveOutWrite) {
        generate_audio_frame(audio_buffers[audio_buf_idx]);
        fn_waveOutWrite(h_wave_out, &wave_hdrs[audio_buf_idx], sizeof(WAVEHDR));
        audio_buf_idx = (audio_buf_idx + 1) % AUDIO_NUM_BUFFERS;
    }
}