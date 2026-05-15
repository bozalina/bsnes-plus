#include "w32_socket.h"

class Tracer : public QObject {
  Q_OBJECT

public:
  void stepCpu();
  void stepSmp();
  void stepSa1();
  void stepSfx();
  void stepSgb();

  Tracer();
  ~Tracer();

public slots:
  void setCpuTraceState(int);
  void setSmpTraceState(int);
  void setSa1TraceState(int);
  void setSfxTraceState(int);
  void setSgbTraceState(int);
  void setTraceMaskState(bool);

  void resetTraceState();

  void flushTraceOutput();

private:
  void setTraceState(bool);

  file tracefile;
  BufferedServer traceServer;

  bool traceCpu;
  bool traceSmp;
  bool traceSa1;
  bool traceSfx;
  bool traceSgb;
  bool traceMask;

  uint8_t *traceMaskCPU;
  uint8_t *traceMaskSMP;
  uint8_t *traceMaskSA1;
  uint8_t *traceMaskSFX;
  uint8_t *traceMaskSGB;

  void outputTrace(const char *buf, int len);
  void outputTraceToSocket(const char *buf, int len);
  void outputTraceToFile(const char *buf, int len);

  void ensureTraceOutputReady();
  void ensureTraceOutputShutdown();

  void outputCpuTrace();
  void outputSmpTrace();
  void outputSa1Trace();
  void outputSfxTrace();
  void outputSgbTrace();

#if defined(DEBUGGER)
  void checkSplitSource(uint32_t wramPc, uint32_t romAddr);
  void outputPatchRecord(uint32_t templateAddr, uint32_t sourceAddr);
#endif
};

extern Tracer *tracer;
