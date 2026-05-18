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

  // CPU trace dedupe is keyed by (address, effective M flag, effective X flag).
  // Same byte can produce different valid disassemblies under 8-vs-16-bit M/X,
  // so we keep one bitmap per (M,X) combo (index = M*2 + X).
  uint8_t *traceMaskCPU[4];
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
};

extern Tracer *tracer;
