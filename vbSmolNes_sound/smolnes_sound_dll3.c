#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PULL mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

// ========================================================
// 오디오 파라미터 및 Win32 waveOut 동적 바인딩
// ========================================================
#define AUDIO_SAMPLE_RATE 44100
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 60) // 735 샘플
#define AUDIO_NUM_BUFFERS 4                        // 4중 버퍼로 언더런(끊김) 완전 방지

#define WAVE_MAPPER     ((unsigned int)-1)
#define WAVE_FORMAT_PCM 1
#define MMSYSERR_NOERROR 0
#define WHDR_DONE       0x00000001

typedef void *HWAVEOUT;

#pragma pack(push, 1)
typedef struct {
    unsigned short wFormatTag;
    unsigned short nChannels;
    unsigned long  nSamplesPerSec;
    unsigned long  nAvgBytesPerSec;
    unsigned short nBlockAlign;
    unsigned short wBitsPerSample;
    unsigned short cbSize;
} WAVEFORMATEX;

typedef struct wavehdr_tag {
    char          *lpData;
    unsigned long  dwBufferLength;
    unsigned long  dwBytesRecorded;
    unsigned long  dwUser;
    unsigned long  dwFlags;
    unsigned long  dwLoops;
    struct wavehdr_tag *lpNext;
    unsigned long  reserved;
} WAVEHDR;
#pragma pack(pop)

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

static PulseChannel pulse1, pulse2;
static TriangleChannel triangle;
static NoiseChannel noise;

// 노이즈 주파수 타이머 룩업 테이블 (NTSC 표준)
static const uint16_t noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
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

        // ----------------------------------------------------
        // 증폭 믹서 (음량을 크게 보정: 최대 16비트 범위 80% 활용)
        // ----------------------------------------------------
        double final_out = (pulse_sample * 850.0) + (tri_sample * 1100.0) + (noise_sample * 750.0);

        if (final_out > 31000.0) final_out = 31000.0;
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
    rombuf[1024 * 1024];

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

static uint32_t chr_mask = 0; // 전역 추가

static uint8_t *get_chr_byte(uint16_t a) {
    uint32_t offset = (chr[a >> chrbits] << chrbits) | (a & ((1 << chrbits) - 1));
    if (chrrom != chrram && chr_mask > 0) {
        offset &= chr_mask;
    }
    return &chrrom[offset];
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
            uint8_t *r =
                V < 8192 ? (write && chrrom != chrram ? &tmp : get_chr_byte(V))
                : V < 16128 ? get_nametable_byte(V)
                : palette_ram + (uint8_t)((V & 19) == 16 ? V ^ 16 : V);
            write ? *r = val : (ppubuf = *r);
            V += ppuctrl & 4 ? 32 : 1;
            V %= 16384;
            return tmp;
        }

        if (write) {
            switch (lo) {
            case 0: ppuctrl = val; T = T & 0xf3ff | val % 4 << 10; break;
            case 1: ppumask = val; break;
            case 5: T = (W ^= 1) ? fine_x = val & 7, T & ~31 | val / 8
                                 : T & 0x8c1f | val % 8 << 12 | val * 4 & 0x3e0; break;
            case 6: T = (W ^= 1) ? T & 0xff | val % 64 << 8 : (V = T & ~0xff | val); break;
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
        if (write) {
            if (lo == 20) {
                for (uint16_t i = 256; i--;) oam[i] = mem(i, val, 0, 0);
            }
            // APU 레지스터 핸들링
            switch (lo) {
            // Pulse 1
            case 0: pulse1.duty = (val >> 6) & 3; pulse1.volume = val & 15; break;
            case 2: pulse1.timer_period = (pulse1.timer_period & 0x0700) | val; break;
            case 3:
                pulse1.timer_period = (pulse1.timer_period & 0x00FF) | ((val & 7) << 8);
                pulse1.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                pulse1.phase = 0;
                break;
            // Pulse 2
            case 4: pulse2.duty = (val >> 6) & 3; pulse2.volume = val & 15; break;
            case 6: pulse2.timer_period = (pulse2.timer_period & 0x0700) | val; break;
            case 7:
                pulse2.timer_period = (pulse2.timer_period & 0x00FF) | ((val & 7) << 8);
                pulse2.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                pulse2.phase = 0;
                break;
            // Triangle
            case 8: triangle.linear_counter = val & 0x7F; break;
            case 10: triangle.timer_period = (triangle.timer_period & 0x0700) | val; break;
            case 11:
                triangle.timer_period = (triangle.timer_period & 0x00FF) | ((val & 7) << 8);
                triangle.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                triangle.phase = 0;
                break;
            // Noise ($400C, $400E, $400F)
            case 12: noise.volume = val & 15; break;
            case 14:
                noise.mode = (val >> 7) & 1;
                noise.timer_period = noise_period_table[val & 15];
                break;
            case 15:
                noise.length_counter = ((val >> 3) > 0) ? (val >> 3) * 4 : 24;
                break;
            // APU Status ($4015)
            case 21:
                pulse1.enabled = val & 1;
                pulse2.enabled = (val >> 1) & 1;
                triangle.enabled = (val >> 2) & 1;
                noise.enabled = (val >> 3) & 1;
                if (!pulse1.enabled) pulse1.length_counter = 0;
                if (!pulse2.enabled) pulse2.length_counter = 0;
                if (!triangle.enabled) triangle.length_counter = 0;
                if (!noise.enabled) noise.length_counter = 0;
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
            case 4: { // MMC3 Bank select / data
                uint8_t addr1 = addr & 1; // <-- 이 줄 추가
                *(addr1 ? &mmc3_chrprg[mmc3_bits & 7] : &mmc3_bits) = val;
                
                int chr_invert = (mmc3_bits >> 7) & 1; // 비트 7: CHR A12 반전
                int prg_mode   = (mmc3_bits >> 6) & 1; // 비트 6: PRG 모드

                // 2KB 뱅크 2개 (R0, R1)
                uint8_t r0 = mmc3_chrprg[0] & ~1;
                uint8_t r1 = mmc3_chrprg[1] & ~1;
                // 1KB 뱅크 4개 (R2, R3, R4, R5)
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

                // PRG 뱅크 (R6, R7)
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
            case 3: chr[0] = val % 4 * 2; chr[1] = chr[0] + 1; break;
            case 2: prg[0] = val & 31; break;
            case 1:
                if (val & 0x80) {
                    mmc1_bits = 5; mmc1_data = 0; mmc1_ctrl |= 12;
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

    memset(rombuf, 0, sizeof(rombuf));
    size_t read_bytes = fread(rombuf, 1, sizeof(rombuf), fp);
    fclose(fp);

    if (read_bytes < 16 || memcmp(rombuf, "NES\x1A", 4) != 0) return 0;

    // ========================================================
    // [핵심] 이전 게임 잔여 데이터 완벽 초기화
    // ========================================================
    memset(ram, 0, sizeof(ram));              // CPU RAM (8KB)
    memset(vram, 0, sizeof(vram));            // PPU VRAM (Name Tables)
    memset(palette_ram, 0, sizeof(palette_ram));// 팔레트 RAM
    memset(oam, 0, sizeof(oam));              // 스프라이트 OAM (256바이트)
    memset(prgram, 0, sizeof(prgram));        // 카트리지 PRG-RAM (8KB)
    memset(frame_buffer, 0, sizeof(frame_buffer)); // 화면 프레임 버퍼

    // CHR-RAM 게임이었던 경우를 대비해 초기화
    memset(chrram, 0, sizeof(chrram));

    // 매퍼 내부 레지스터 초기화
    mmc1_bits = 5;
    mmc1_data = 0;
    mmc1_ctrl = 0x0C; // 기본 16KB PRG 뱅크 모드
    chrbank0 = chrbank1 = prgbank = 0;

    memset(mmc3_chrprg, 0, sizeof(mmc3_chrprg));
    mmc3_bits = mmc3_irq = mmc3_latch = 0;

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

    // 롬 헤더 파싱 및 뱅크 기본값 설정
    rom = rombuf + 16;
    prg[0] = 0;
    prg[1] = rombuf[4] - 1;
    chrrom = rombuf[5] ? rom + (rombuf[4] << 14) : chrram;
    chr[0] = 0;
    chr[1] = rombuf[5] ? rombuf[5] * 2 - 1 : 1;
    mirror = 3 - rombuf[6] % 2;
    prgbits = 14;
    chrbits = 12;

    if (rombuf[5] > 0) {
        chr_mask = (rombuf[5] * 8192) - 1;
    } else {
        chr_mask = 8191;
    }

    if (rombuf[6] / 16 == 4) { // MMC3
        mem(0, 128, 0, 1);
        prgbits--;
        chrbits -= 2;
    }

    // CPU 리셋 벡터 점프
    pc_l = mem(~3, ~0, 0, 0);
    pc_h = mem(~2, ~0, 0, 0);

    // APU 오디오 상태 초기화
    memset(&pulse1, 0, sizeof(pulse1));
    memset(&pulse2, 0, sizeof(pulse2));
    memset(&triangle, 0, sizeof(triangle));
    memset(&noise, 0, sizeof(noise));
    noise.shift_reg = 1;

    return 1;
}

__declspec(dllexport) int __stdcall NES_LoadROM0(const char* filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return 0;

    memset(rombuf, 0, sizeof(rombuf));
    size_t read_bytes = fread(rombuf, 1, sizeof(rombuf), fp);
    fclose(fp);

    if (read_bytes < 16 || memcmp(rombuf, "NES\x1A", 4) != 0) return 0;

    rom = rombuf + 16;
    prg[1] = rombuf[4] - 1;
    chrrom = rombuf[5] ? rom + (rombuf[4] << 14) : chrram;
    chr[1] = rombuf[5] ? rombuf[5] * 2 - 1 : 1;
    mirror = 3 - rombuf[6] % 2;
    prgbits = 14;
    chrbits = 12;
   
   // rombuf[5]는 8KB CHR 페이지 수
    if (rombuf[5] > 0) {
        chr_mask = (rombuf[5] * 8192) - 1;
    } else {
        chr_mask = 8191; // CHR-RAM
    }

    if (rombuf[6] / 16 == 4) {
        mem(0, 128, 0, 1);
        prgbits--;
        chrbits -= 2;
    }

    P = 4;
    S = ~2;
    pc_l = mem(~3, ~0, 0, 0);
    pc_h = mem(~2, ~0, 0, 0);
    dot = scany = nmi_irq = W = 0;

    // APU 초기화
    memset(&pulse1, 0, sizeof(pulse1));
    memset(&pulse2, 0, sizeof(pulse2));
    memset(&triangle, 0, sizeof(triangle));
    memset(&noise, 0, sizeof(noise));
    noise.shift_reg = 1;

    return 1;
}

__declspec(dllexport) void __stdcall NES_SetInput(uint8_t buttons) {
    vb_key_state = buttons;
}

/*
__declspec(dllexport) uint32_t* __stdcall NES_GetBuffer(void) {
    return frame_buffer;
}
*/
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

    if (fn_waveOutOpen(&h_wave_out, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) {
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
                        case 5: ptb_lo = *get_chr_byte(temp); break;
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

                if ((scany + 1) % 262 < 241 && dot == 261 && mmc3_irq && !mmc3_latch--) nmi_irq = 1;
                if (scany == 261 && dot - 280 < 25u) V = V & 0x841f | T & 0x7be0;
            }

            if (dot == 1) {
                if (scany == 241) {
                    if (ppuctrl & 128) nmi_irq = 4;
                    ppustatus |= 128;
                    frame_done = 1;
                }
                if (scany == 261) ppustatus = 0;
            }

            if (++dot == 341) { dot = 0; scany++; scany %= 262; }
        }
    }
     
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