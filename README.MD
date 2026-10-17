# OBS-Seeker 7.5.0

On Windows, Seeker preloads PowerPoint once after OBS starts. No presentation is opened, and no setting is required. PowerPoint stays loaded while OBS is running; closing OBS releases the preload reference without calling Quit. If preloading fails, file opening keeps its usual behavior. Mac does not preload PowerPoint.

**Present and switch scene** is on by default. Choose a scene and save to enable scene switching. PowerPoint PPT/PPTX and Mac Keynote KEY use automatic presentation. The current Keynote is preferred; classic Keynote remains supported. PPS/PPSX and other configured types use the default app. The scene delay is **0.5–3 seconds**, default **0.5 seconds**. No scene is selected initially.

Windows PPT/PPTX opens read-only. Seeker does not save files or discard existing edits. Failed, canceled or timed-out presentation commands do not switch scenes. If automation cannot open the file, Seeker tries the default app once, without switching scenes.

Esc does not close OBS fullscreen projectors. Use the projector's context menu to close it. Windowed projectors and other windows keep their usual Esc behavior.

**Esc to HOME** is on by default. The global shortcut returns to the scene named exactly `HOME`; OBS inputs, menus and dialogs are excluded. In a foreground fullscreen PowerPoint or Mac Keynote presentation, Esc switches to HOME and exits the presentation after the configured delay, without waiting for the transition to finish. If HOME is missing, Esc passes through. OBS hotkey focus settings apply to the ordinary HOME shortcut.

On Mac, allow **Seeker Presentation** to control PowerPoint/Keynote when prompted. Esc interception also needs **OBS** in System Settings → Privacy & Security → Accessibility; restart OBS or save Seeker settings again after granting access. Ending the slideshow may leave the document's editing window open.

Automatic presentation checks PowerPoint/Keynote automation access after the app starts. If access is denied, Seeker uses normal opening without switching scenes. On the next OBS startup, it resets this helper's automation permissions once and requests access again for both installed apps. You must still choose Allow. With no recorded denial, existing permissions remain unchanged.

Build outputs go to `dist/`, which is cleared before each build. Source archives exclude `dist/`.
Installers close after five seconds; press any key to exit sooner.

## Windows

Copy the `OBS-Seeker` folder to:

```text
C:\ProgramData\obs-studio\plugins\
```

## macOS · Apple Silicon

Copy `OBS-Seeker.plugin` to:

```text
~/Library/Application Support/obs-studio/plugins/
```
