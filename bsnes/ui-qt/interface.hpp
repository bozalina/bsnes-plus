class Interface : public SNES::Interface {
public:
  void video_extras(uint16_t *data, unsigned width, unsigned height);
  void video_refresh(const uint16_t *data, unsigned width, unsigned height);
  void audio_sample(uint16_t left, uint16_t right);
  void input_poll();
  int16_t input_poll(bool port, SNES::Input::Device device, unsigned index, unsigned id);
  void message(const string &text);

  Interface();
  void captureScreenshot(const QImage&);
  void captureSPC();
  // Writes the most recent rendered frame as PNG to an explicit path.
  // Returns true on success, false if no frame has been rendered yet.
  bool saveScreenToFile(const string& path);

  // Programmatic controller override. When active, input_poll returns these
  // button states for port 1 (Joypad) instead of physical input.
  // Bit positions match SNES::Input::JoypadID values (B=0..R=11).
  bool     inputOverrideActive  = false;
  uint16_t inputOverrideButtons = 0;
  void setInputOverride(uint16_t buttons);
  void clearInputOverride();

  bool saveScreenshot;
  bool framesUpdated;
  unsigned framesExecuted;
  QImage lastFrame;   // most recent rendered frame, kept for paused screen dumps
};

extern Interface interface;
