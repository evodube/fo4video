Scriptname FO4Video Native Hidden
{Play video files on in-game screens. Requires the fo4video F4SE plugin.
Video files live in Data\Videos (see fo4video.ini). File names may omit the folder and the .mp4 extension.}

; Start playing a video on the screen of akScreen (a TV, monitor, projector screen...).
; Returns false if the file doesn't exist. Playback starts on the next frame.
bool Function Play(ObjectReference akScreen, string asFile, bool abLoop = true) global native

; Show an animated test pattern (useful to check that a screen can be found).
bool Function PlayTestPattern(ObjectReference akScreen) global native

Function Stop(ObjectReference akScreen) global native
Function StopAll() global native
bool Function IsPlaying(ObjectReference akScreen) global native

; 0.0 - 4.0, multiplied with the INI volume and distance falloff.
Function SetVolume(ObjectReference akScreen, float afVolume) global native

; Writes akScreen's mesh tree to Documents\My Games\Fallout4\F4SE\fo4video.log
Function DumpNodes(ObjectReference akScreen) global native

; Number of video files in the video folder.
int Function GetVideoCount() global native
