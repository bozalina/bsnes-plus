class State {
public:
  unsigned active;
  bool save(unsigned);
  bool load(unsigned);
  bool loadFromPath(const char *path);

  void frame();
  void resetHistory();
  bool rewind();

  State();
  ~State();

private:
  serializer *history;
  unsigned historySize;
  unsigned historyIndex;
  unsigned historyCount;
  unsigned frameCounter;

  bool allowed() const;
  string name(unsigned slot) const;
};

extern State state;
