---
{
  "name": "mpx_music",
  "description": "Play music on the robot's speaker: internet radio by genre or mood, or a direct audio URL.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_music"
    ],
    "manage_mode": "readonly"
  }
}
---

# Music on the robot

`music_play` plays music on the robot's speaker until `music_stop`.

- **A genre or mood** (`{"genre": "jazz"}`) picks a commercial-free internet
  radio station (SomaFM): chill, pop, indie, 80s, 70s, jazz, lounge, soul,
  funk, hip hop, house, country, folk, reggae, metal, trance, ambient, sleep,
  covers.
- **Nothing** (`{}`) picks a random cheerful station: use it for "play a
  song" or "play some music".
- **A direct link** (`{"url": "https://.../song.mp3"}`) plays that stream or
  file (mp3, aac, m4a, flac, wav). A web page is not an audio link.

It is radio. It cannot fetch a named song by title from a catalogue. If you
are asked for a specific song, either play a station in that song's genre
and say so, or find a direct, legal audio URL for it and pass that.

Call `music_play` again to change station. The music waits while someone is
talking to the robot and starts as soon as the conversation is over.

**Do not use the Lua `audio` module on this robot.** Its speaker and
microphone share one I2S clock, and opening the speaker from Lua reclocks the
microphone (the wake word stops working) or plays at the wrong speed. The
music tools play through the robot's own audio path, at the right rate and a
safe level for its small speaker.
