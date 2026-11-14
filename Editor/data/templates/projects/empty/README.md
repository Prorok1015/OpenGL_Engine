# Game config as a data document

`code/src/game_module.cpp` declares `game_config` (player speed, goal) and marks it as a data document with
`layer.descs().register_document<game_config>("game.config")`. `res/config/game.desc` is a file of that type.

- **Edit it:** build the game (Build > Build Game), then double click `config/game.desc` in the Asset Browser. The
  inspector shows the fields; save with Ctrl+S. More documents of the type: right click a folder > Create Document...
- **Read it:** `read_game_config` asks the resource system for `game_config` by the tag every frame (a hit of the cache while
  the file is unchanged) and keeps the value in `game_config_state`, which any system of the game can take as a parameter.
  List `game::read_config` in the `systems` of a world in `res/levels/main.desc` to run it.
- **Play sees a change after the save,** not while it is edited: a save makes the next request load the file again, so the
  value changes within a frame or two and the library is not rebuilt.
- **More fields:** add them to `game_config` (the names in `BOOST_DESCRIBE_CLASS`) and rebuild the library once; a file that
  lacks a field gets its default.
