void Application::init() {
  if(config().system.crashedOnLastRun == true) {
    //Previous run did not shut down cleanly (e.g. killed mid-init, or a driver
    //init crash). This build is driven headlessly through the debugger API, so a
    //blocking modal here would deadlock startup and prevent the API server from
    //ever coming up. Log and continue -- the per-driver init checks below still
    //fall back to "None" for any driver that genuinely fails to initialize.
    fprintf(stderr, "[bsnes] previous run did not exit cleanly; continuing "
                    "(a failing driver will fall back to None)\n");
  }

  // "Carbon" driver is retired, fallback to default if encountered in serialized settings
  if(config().system.input == "Carbon") config().system.input = input.default_driver();

  // Default drivers
  if(config().system.video == "") config().system.video = video.default_driver();
  if(config().system.audio == "") config().system.audio = audio.default_driver();
  if(config().system.input == "") config().system.input = input.default_driver();

  loaderWindow = new LoaderWindow;
  htmlViewerWindow = new HtmlViewerWindow;
  aboutWindow = new AboutWindow;
  fileBrowser = new FileBrowser;
  stateSelectWindow = new StateSelectWindow;

  #if defined(DEBUGGER)
  debugger = new Debugger;
  #endif
  settingsWindow = new SettingsWindow;
  toolsWindow = new ToolsWindow;
  soundViewerWindow = new SoundViewerWindow;

  //window must be onscreen and visible before initializing video interface
  mainWindow = new MainWindow;
  utility.updateSystemState();
  utility.resizeMainWindow();
  utility.updateFullscreenState();
  QApplication::processEvents();

  //if emulator crashes while initializing drivers, next run will disable them all.
  //this will allow user to choose different driver settings.
  config().system.crashedOnLastRun = true;
  config().save(configFilename);

  video.driver(config().system.video);
  video.set(Video::Handle, (uintptr_t)mainWindow->canvas->winId());
  video.set("QWidget", (QWidget*)mainWindow->canvas);
  if(video.init() == false) {
    fprintf(stderr, "[bsnes] %s video driver failed to initialize; using None.\n",
            (const char*)config().system.video);
    video.driver("None");
    video.init();
  }

  audio.driver(config().system.audio);
  audio.set(Audio::Handle, (uintptr_t)mainWindow->canvas->winId());
  audio.set(Audio::Frequency, config().audio.outputFrequency);
  audio.set(Audio::Latency, config().audio.latency);
  audio.set(Audio::Volume, config().audio.volume);
  if(audio.init() == false) {
    fprintf(stderr, "[bsnes] %s audio driver failed to initialize; using None.\n",
            (const char*)config().system.audio);
    audio.driver("None");
    audio.init();
  }

  input.driver(config().system.input);
  input.set("Handle", (uintptr_t)mainWindow->canvas->winId());
  if(input.init() == false) {
    fprintf(stderr, "[bsnes] %s input driver failed to initialize; using None.\n",
            (const char*)config().system.input);
    input.driver("None");
    input.init();
  }

  //didn't crash, note this in the config file now in case a different kind of crash occurs later
  config().system.crashedOnLastRun = false;
  config().save(configFilename);

  //hide pixel shader settings if driver does not support them
  videoSettingsWindow->synchronizePixelShaderSettings();

  utility.resizeMainWindow();
  utility.updateAvSync();
  utility.updateColorFilter();
  utility.updatePixelShader();
  utility.updateHardwareFilter();
  utility.updateSoftwareFilter();
  utility.updateEmulationSpeed();
  utility.updateControllers();
}
