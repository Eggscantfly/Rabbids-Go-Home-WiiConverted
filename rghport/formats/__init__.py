"""Record formats of Rabbids Go Home's bigfiles, one stream description per type (see stream.py).

    stream      the framework (Reader / Writer / parse / emit) and the core streams: MDF header, world, object, VIS,
                materials, texture descriptors
    geometry    K3D visuals (meshes, GOM chains, LODs)
    texture     texture bank pixel layouts
    curve       MTH curves
    vif         VIF visual instances
    zones       GRP, ZDE, COB, VP, OCC, TRG and their resources
    effects     LIG, DYN, VEG, LRP, LIP, CAM, LG, EFF, PAR and particle models
    modifiers   SNK, SNF, SNL, TXT, MSSG, TBO, DST and their resources
    sound       SLib sound banks and files (sets, smp, smx, mic, variables, samples)
    text        text groups, language files, dialogs, fonts, language bins
    world       NET, AFX, RDP, MGM, BVO modifiers and Magma file descriptors
    anim        SKL, skin models, ACT, action kits, actions, morph kits, SP, ANI, EVE, track lists
    edge        Edge animation and skeleton blobs
    script      script instances and script model records
"""
