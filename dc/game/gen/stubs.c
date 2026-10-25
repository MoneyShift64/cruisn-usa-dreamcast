/* generated: functions and data the port does not have yet */
void port_stub(const char *name);
static unsigned char gStubObj[64][0x400] __attribute__((aligned(16)));
static int gStubNext;
static void *stub_obj(void) { return gStubObj[gStubNext++ & 63]; }
unsigned char D_8013B2C0[0x1000] __attribute__((aligned(16)));
unsigned char D_8013B2C8[0x1000] __attribute__((aligned(16)));
unsigned char D_8013B2CC[0x1000] __attribute__((aligned(16)));
unsigned char D_8013B2D0[0x1000] __attribute__((aligned(16)));
unsigned char D_8013B2DC[0x1000] __attribute__((aligned(16)));
unsigned char D_8013B2E0[0x1000] __attribute__((aligned(16)));
int audioBuildTask(void) { static int n; if (!n++) port_stub("audioBuildTask"); return 0; }
int dmaNotifyDone(void) { static int n; if (!n++) port_stub("dmaNotifyDone"); return 0; }
int dmaRunCallbacks(void) { static int n; if (!n++) port_stub("dmaRunCallbacks"); return 0; }
int fatalError(void) { static int n; if (!n++) port_stub("fatalError"); return 0; }
int huffStart(void) { static int n; if (!n++) port_stub("huffStart"); return 0; }
int huffStep(void) { static int n; if (!n++) port_stub("huffStep"); return 0; }
int intDisable(void) { static int n; if (!n++) port_stub("intDisable"); return 0; }
int intRestore(void) { static int n; if (!n++) port_stub("intRestore"); return 0; }
int osContGetQuery(void) { static int n; if (!n++) port_stub("osContGetQuery"); return 0; }
int osContGetReadData(void) { static int n; if (!n++) port_stub("osContGetReadData"); return 0; }
int osContStartQuery(void) { static int n; if (!n++) port_stub("osContStartQuery"); return 0; }
int osContStartReadData(void) { static int n; if (!n++) port_stub("osContStartReadData"); return 0; }
int osCreateMesgQueue(void) { static int n; if (!n++) port_stub("osCreateMesgQueue"); return 0; }
int osGetTime(void) { static int n; if (!n++) port_stub("osGetTime"); return 0; }
int osInvalDCache(void) { static int n; if (!n++) port_stub("osInvalDCache"); return 0; }
int osInvalICache(void) { static int n; if (!n++) port_stub("osInvalICache"); return 0; }
int osPiStartDma(void) { static int n; if (!n++) port_stub("osPiStartDma"); return 0; }
int osRecvMesg(void) { static int n; if (!n++) port_stub("osRecvMesg"); return 0; }
int osSendMesg(void) { static int n; if (!n++) port_stub("osSendMesg"); return 0; }
int osSetEventMesg(void) { static int n; if (!n++) port_stub("osSetEventMesg"); return 0; }
int osSetTimer(void) { static int n; if (!n++) port_stub("osSetTimer"); return 0; }
int osSpTaskLoad(void) { static int n; if (!n++) port_stub("osSpTaskLoad"); return 0; }
int osSpTaskStartGo(void) { static int n; if (!n++) port_stub("osSpTaskStartGo"); return 0; }
int osSpTaskYield(void) { static int n; if (!n++) port_stub("osSpTaskYield"); return 0; }
int osSpTaskYielded(void) { static int n; if (!n++) port_stub("osSpTaskYielded"); return 0; }
int osViBlack(void) { static int n; if (!n++) port_stub("osViBlack"); return 0; }
int osViGetCurrentFramebuffer(void) { static int n; if (!n++) port_stub("osViGetCurrentFramebuffer"); return 0; }
int osViSetMode(void) { static int n; if (!n++) port_stub("osViSetMode"); return 0; }
int osViSetSpecialFeatures(void) { static int n; if (!n++) port_stub("osViSetSpecialFeatures"); return 0; }
int osViSwapBuffer(void) { static int n; if (!n++) port_stub("osViSwapBuffer"); return 0; }
int osWritebackDCache(void) { static int n; if (!n++) port_stub("osWritebackDCache"); return 0; }
int osWritebackDCacheAll(void) { static int n; if (!n++) port_stub("osWritebackDCacheAll"); return 0; }
