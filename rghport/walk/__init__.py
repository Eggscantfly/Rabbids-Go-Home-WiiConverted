"""The world and list walker that assigns a kind to every package record of the Wii archive.

    registry    which stream reads which modifier / resource type and what each record points at
    walker      the walk from world lists and world groups down to every record they load
    kinds       the kinds of a whole archive (all lists and global groups, script records), cached; the probes that
                type records no walk reaches
"""
