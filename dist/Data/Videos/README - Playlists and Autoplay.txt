REAL VIDEOS - PLAYLISTS AND AUTOPLAY
====================================

Put your videos in this folder (Data\Videos). Mod Organizer users can also put
them in a mod of their own, under Data\Videos - the game sees them the same way.

Video types: .mp4 .m4v .wmv .avi .mkv .webm (MP4 works best)

Open the remote with F6 while aiming at a TV, projector or cinema screen.


FOLDERS
-------
You can sort your videos into folders. Real Videos treats each folder as its
own list:

  - Next and Previous stay inside the folder of the video that's playing.
  - Home goes back to the first video in that folder.
  - Videos play in order of their file names. To set the order, number them:
      01 Intro.mp4, 02 Part One.mp4, 03 Part Two.mp4 ...


PLAYLISTS
---------
Any folder inside Data\Videos\Playlists shows up as a playlist on the remote.
The folder's name is the playlist's name.

  Data\Videos\
      Playlists\
          Horror\
              01 Night of the Ghouls.mp4
              02 The Glowing Sea.mp4
          Cartoons\
              ...
          Autoplay\
              ...

To use them, open the PLAYLISTS tab on the right edge of the remote:

  - Play          starts the playlist from its first video.
  - Click a name  opens it, so you can pick any video in it.

The playlist that's on now is highlighted.

Folders inside a playlist are listed under that playlist, but Next and Previous
stay inside the exact folder the video is in.


SHUFFLE
-------
Turn on "Shuffle" on the remote's Settings page (or bShuffle=1 in
RealVideos.ini) to play videos in random order.

  - Shuffle only picks from the folder the screen is playing from. Folders
    never mix, so a Cartoons screen stays on cartoons.
  - Next picks a random video, and when a video ends another random one starts.
  - Previous goes back to the video you saw before.
  - Turning a screen on picks a random video from the folder it last played.
    If it hasn't played anything yet, it picks from the videos directly in
    Data\Videos (not in a folder).


AUTOPLAY
--------
Data\Videos\Playlists\Autoplay is a special playlist. Use it for screens that
should feel like live TV - a channel that's always on something new.

Screens normally remember where they were: walk away and come back (or load a
save) and the video carries on from the same spot. Autoplay screens don't.
When you come back, the screen starts a different random video from the
Autoplay folder, from the beginning.

  - Start the screen on the Autoplay playlist once, using the remote. Autoplay
    doesn't turn screens on by itself.
  - Folders inside Autoplay work too, and each one stays separate. For example,
    Autoplay\News and Autoplay\Music: a screen on News only ever picks from
    News.
  - If a folder has only one video, it starts again from the beginning.


VIDEO POSTERS
-------------
The Contraptions Workshop powered posters have their own videos, kept apart
from everything else. Aim at a powered poster and press F6.

  Data\Videos\
      Posters\              videos for posters (portrait 9:16 fits best)
          Playlists\
              Ads\          a poster playlist
              Autoplay\     the posters' own Autoplay playlist

Posters only list Data\Videos\Posters and its Playlists. TVs, projectors and
the cinema never list anything in Posters, and keep using Data\Videos and
Data\Videos\Playlists.


QUICK SETUP
-----------
  1. Make the folders: Data\Videos\Playlists\<any name>
  2. Put videos in them.
  3. In game, aim at a screen, press F6, open the PLAYLISTS tab, press Play.

Videos added while the game is running show up the next time you open the
remote.
