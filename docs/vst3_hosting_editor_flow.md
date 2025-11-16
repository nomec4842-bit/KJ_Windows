# VST3 3.8 Host Editor Flow (based on VSTHost behavior)

The following sequence adapts VSTHost's editor-handling ideas to the modern VST3 SDK 3.8 interfaces. It shows how a Windows host can load a VST3 module, create the component and controller, and manage the plug-in's GUI lifecycle using the current APIs.

## 1. Load the VST3 module
1. Use `Steinberg::FUnknown* moduleHandle = nullptr;` and call the module entry point exported by the plug-in (e.g., `GetPluginFactory`) after loading the module with `LoadLibraryW`.
2. Retrieve the factory via `IPluginFactory3* factory = Steinberg::VST3::GetPluginFactoryFromModule(moduleHandle);` using SDK helpers, or by calling `GetPluginFactory` and querying for `IPluginFactory3`.
3. Confirm the factory supports the desired class IDs by enumerating `factory->countClasses()` and `factory->getClassInfo2(...)`.

## 2. Instantiate component and controller
1. Identify the audio component class (usually implementing `kVstAudioEffectClass`) from the factory info.
2. Call `factory->createInstance(classCID, IComponent::iid, (void**)&component)` to create the processor component.
3. Query the same factory (or the component) for the controller class ID via `component->getControllerClassId(controllerCID)`.
4. Create the controller with `factory->createInstance(controllerCID, IEditController::iid, (void**)&controller)`.

## 3. Connect component and controller
1. Call `component->initialize(<IHostApplication*>)` and `controller->initialize(<IHostApplication*>)` with your host implementation.
2. Connect both sides using `component->setIoMode(kRealtime)` (as needed) and `component->setActive(true)`.
3. Provide the component with the controller connection by calling `component->setControllerClass(controllerCID)` (if required) and then use `component->connect(controller)` / `controller->setComponentHandler(...)` with your host's handler implementation to manage parameter updates.
4. Complete setup with `component->setupProcessing(ProcessSetup{...})` and `component->setProcessing(true)` once audio is ready.

## 4. Create the plug-in editor view
1. Request the editor from the controller via `IPlugView* view = controller->createView(ViewType::kEditor);` (use the literal string "editor").
2. Ensure `view` is valid and call `view->setFrame(<IPlugFrame* provided by host>)` to supply frame callbacks.

## 5. Query preferred size
1. Prepare a `ViewRect preferred = {0, 0, 0, 0};` and call `view->getSize(&preferred)`.
2. If the plug-in returns `kResultTrue`, compute width/height from `preferred.right - preferred.left` and `preferred.bottom - preferred.top`.
3. Size your native host window to match before attaching the view.

## 6. Attach the view to a Win32 parent
1. Create a child `HWND` with styles `WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN`.
2. Attach the plug-in view using `view->attached((void*)parentHwnd, PlatformType::kPlatformTypeHWND);`.
3. After attachment, call `view->onSize(width, height)` if you resized the parent to the preferred dimensions.

## 7. Run idle and message loop
1. In your host's idle loop (e.g., during `WM_TIMER` or custom tick), call `view->onIdle()` to let the plug-in repaint and process GUI events.
2. Continue pumping the Win32 message loop with `GetMessage`/`TranslateMessage`/`DispatchMessage` so the child window receives messages.
3. When closing, call `view->removed()` before destroying the child window and then release the view, controller, and component in reverse initialization order.

These steps preserve VSTHost's parenting and sizing behavior while using the current VST3 3.8 API surface.
