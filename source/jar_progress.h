#ifndef JAR_PROGRESS_H
#define JAR_PROGRESS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Renders the native JAR-import progress on a separate Switch VI layer.
 * The overlay is owned by the wrapper's applet thread, not the Godot/game
 * thread, so a synchronous import cannot freeze the progress display.
 */
void jar_progress_overlay_update(void);
void jar_progress_overlay_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
