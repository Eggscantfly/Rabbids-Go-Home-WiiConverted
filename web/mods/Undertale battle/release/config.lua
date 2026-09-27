-- The battle's own settings.  This is the mod's file, not the platform's: change it and restart the game.
return {
  -- the folder of your own copy of Undertale: that game's data file and songs are read from there while you
  -- play.  This is where Steam puts it; if yours is somewhere else, put its folder here.  Or copy data.win and
  -- mus_spider.ogg from it into this mod's folder and write:  undertale = wc.dir,
  undertale = [[C:\Program Files (x86)\Steam\steamapps\common\Undertale]],

  key = "G",                 -- what starts and ends an encounter (a name from wc.vk: G, H, F9 ...)
  music = "mus_spider.ogg",  -- any of that game's songs beside its data.win (mus_battle1, mus_spider ...)
  music_volume = 70,         -- 0..100
  sfx_volume = 85,
  name = "RABBID",           -- the name under the box
  breath_frames = 24,        -- how long the enemy holds each of its two pictures, in that game's frames
}
