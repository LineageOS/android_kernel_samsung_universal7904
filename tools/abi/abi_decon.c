/* Host ABI check: Exynos7885 HWC (DeconHeader.h) vs 4.4 dpu_7885 with CONFIG_DPU_20. */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t dma_addr_t;   /* arm64: 64-bit dma addr */
#define MAX_DECON_WIN 4
#define MAX_PLANE_CNT 3
enum blend_t {B0,B1}; enum idma_t {I0,I1}; enum fmt_t {F0,F1};
enum rot_t {R0,R1}; enum eq_t {E0,E1}; enum comp_t {C0,C1}; enum hdr_t {H0,H1};
struct win_rect { int x; int y; u32 w; u32 h; };
struct frame { int x; int y; u32 w; u32 h; u32 f_w; u32 f_h; };
/* ---- HWC (exynos7885 DeconHeader.h) ---- */
struct hwc_dpp_params { dma_addr_t addr[MAX_PLANE_CNT]; enum rot_t rot; enum eq_t eq_mode; enum comp_t comp_src; enum hdr_t hdr_std; u32 min_luminance; u32 max_luminance; };
struct hwc_win_config {
  enum { HS0, HS1, HS2, HS3, HS4 } state;
  union { u32 color; struct { int fd_idma[3]; int acq_fence; int rel_fence; int plane_alpha;
     enum blend_t blending; enum idma_t idma_type; enum fmt_t format; struct hwc_dpp_params dpp_parm;
     struct win_rect block_area; struct win_rect transparent_area; struct win_rect opaque_area; struct frame src; }; };
  struct frame dst; _Bool protection; _Bool compression;
};
struct hwc_win_config_data { int present_fence; int fd_odma; struct hwc_win_config config[MAX_DECON_WIN + 1]; };
/* ---- 4.4 decon.h, CONFIG_DPU_20 on ---- */
struct k_dpp_params { dma_addr_t addr[MAX_PLANE_CNT]; enum rot_t flip; enum eq_t eq_mode; enum comp_t comp_src; enum hdr_t hdr_std; u32 min_luminance; u32 max_luminance; };
struct k_win_config {
  enum { KS0, KS1, KS2, KS3, KS4 } state;
  union { u32 color; struct { int fd_idma[3]; int fence_fd; int rel_fence; int plane_alpha;
     enum blend_t blending; enum idma_t idma_type; enum fmt_t format; struct k_dpp_params dpp_parm;
     struct win_rect block_area; struct win_rect transparent_area; struct win_rect opaque_area; struct frame src; }; };
  struct frame dst; _Bool protection; _Bool compression;
};
struct k_win_config_extra { int remained_frames; u32 reserved[7]; };
struct k_win_config_data_old { int fence; int fd_odma; struct k_win_config config[MAX_DECON_WIN + 1]; };
struct k_win_config_data { int fence; int fd_odma; struct k_win_config config[MAX_DECON_WIN + 1]; struct k_win_config_extra extra; };
#define IOW(t,n,s) (((unsigned)1<<30)|((sizeof(s))<<16)|((t)<<8)|(n))
#define CHK(name, a, b) printf("%-34s hwc=%-6zu k44=%-6zu %s\n", name, (size_t)(a), (size_t)(b), (a)==(b)?"OK":"MISMATCH")
int main(void) {
  CHK("sizeof(dpp_params)", sizeof(struct hwc_dpp_params), sizeof(struct k_dpp_params));
  CHK("sizeof(win_config)", sizeof(struct hwc_win_config), sizeof(struct k_win_config));
  CHK("offsetof(win_config,dst)", offsetof(struct hwc_win_config,dst), offsetof(struct k_win_config,dst));
  CHK("offsetof(win_config,protection)", offsetof(struct hwc_win_config,protection), offsetof(struct k_win_config,protection));
  CHK("sizeof(config_data)  [HWC vs _old]", sizeof(struct hwc_win_config_data), sizeof(struct k_win_config_data_old));
  CHK("sizeof(config_data)  [HWC vs new]", sizeof(struct hwc_win_config_data), sizeof(struct k_win_config_data));
  CHK("offsetof(config_data,config)", offsetof(struct hwc_win_config_data,config), offsetof(struct k_win_config_data_old,config));
  printf("S3CFB_WIN_CONFIG (209) encoded with HWC struct : 0x%08x\n", IOW('F',209,struct hwc_win_config_data));
  printf("S3CFB_WIN_CONFIG (209) encoded with 4.4 _old   : 0x%08x\n", IOW('F',209,struct k_win_config_data_old));
  printf("S3CFB_WIN_CONFIG (209) encoded with 4.4 new    : 0x%08x\n", IOW('F',209,struct k_win_config_data));
  return 0;
}
