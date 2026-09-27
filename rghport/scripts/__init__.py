"""Scripts phase: everything the converter does with game logic.

  Wii script module   the retail Wii script module (SCR2CPP C++ build of the game scripts): reader (rso), PowerPC
                      disassembly (ppcdis), lifting (rsolift), structuring (rsodec), script source emission
                      (scremit, scrmethod), model tree (scrmodel, scrproject)
  PC bytecode         the PC release's script records: node streams (scrdis), decompiler (scrvm)
  compiler            script source -> PC script records (scrc)
  restoration         Wii logic rebuilt for the PC executable at convert time (scrc_wii and the rule engine)
  tables              natives, keywords, node sizes and engine types, generated from the PC executable (always) and
                      from the Wii executable when the user supplies it (natives)

Every generated file goes to the cache folder (see workspace.py); nothing derived from the game ships with the tool.
"""
