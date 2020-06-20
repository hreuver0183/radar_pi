#ifdef INITIALIZE_RADAR

PLUGIN_BEGIN_NAMESPACE

static const NetworkAddress report(0, 0, 0, 0, 0);
static const NetworkAddress data(0, 0, 0, 0, 0);
static const NetworkAddress send(0, 0, 0, 0, 0);

#define RANGE_METRIC_RM_E120 \
  { 50, 75, 100, 250, 500, 750, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 12000, 16000, 24000, 36000, 48000 }
#define RANGE_MIXED_RM_E120                                                                                                    \
  {                                                                                                                          \
    50, 75, 100, 1852 / 8, 1852 / 4, 1852 / 2, 1852 * 3 / 4, 1852 * 1, 1852 * 3 / 2, 1852 * 2, 1852 * 3, 1852 * 4, 1852 * 6, \
        1852 * 8, 1852 * 12, 1852 * 16, 1852 * 24                                                                            \
  }
#define RANGE_NAUTIC_RM_E120 \
  {1852 / 4, 1852 / 2, 1852, 1852 * 3 / 2, 1852 * 3, 1852 * 6, 1852 * 12, 1852 * 24, 1852 * 48, 1852 * 96, 1852 * 144};

PLUGIN_END_NAMESPACE

#endif

#ifndef RM_E120_SPOKES
#define RM_E120_SPOKES 2048
#endif

#ifndef RM_E120_SPOKE_LEN
#define RM_E120_SPOKE_LEN (512)  // BR radars generate 512 separate values per range, at 8 bits each (according to original RMradar_pi)
#define RETURNS_PER_LINE (512)  // BR radars generate 512 separate values per range, at 8 bits each
#endif

#if SPOKES_MAX < RAYMARINE_SPOKES
#undef SPOKES_MAX
#define SPOKES_MAX RM_E120_SPOKES
#endif
#if SPOKE_LEN_MAX < RM_E120_SPOKE_LEN
#undef SPOKE_LEN_MAX
#define SPOKE_LEN_MAX RM_E120_SPOKE_LEN
#endif

// Raymarine E120 has 2048 spokes of exactly 1024 pixels of 4 bits each, packed in 512 bytes
DEFINE_RADAR(RM_E120,                                                     /* Type */
             wxT("Raymarine E120"),                                       /* Name */
             RM_E120_SPOKES,                                              /* Spokes */
             RM_E120_SPOKE_LEN,                                           /* Spoke length (max) */             
             RME120ControlsDialog(RM_E120),                                 /* ControlsDialog class constructor */
             RME120Receive(pi, ri, report, data, send),                                       /* Receive class constructor */
             RME120Control(),                                             /* Send/Control class constructor */
             RO_SINGLE /* This type only has a single radar and does not need locating */
)
