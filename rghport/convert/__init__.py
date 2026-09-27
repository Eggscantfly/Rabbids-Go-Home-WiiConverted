"""Wii -> PC conversion of the whole archive and assembly of the port folder.

    context     both archives, their record indexes and walked packages
    geometry    K3D visual records (Wii vertex buffers -> PC meshes)
    textures    texture banks and texture header records
    animation   track lists (Edge animations baked into EVE tracks) and skin models
    sound       sound records and bins (Wii audio kept; smx aux blocks rewritten)
    language    language indexes, language bins and text group references
    small       worlds, objects, groups and Magma file descriptors
    builder     one package at a time: overrides, the scripts phase, PC script records, identical kinds, converters
    build       the whole archive into one bigfile
    hooks       the interface the scripts phase (rghport.scripts) plugs into the build
    port        the port folder around it (PC executable, shaders, video library, platform files, sav, sibling
                bigfiles, the launch line) and its check
"""
