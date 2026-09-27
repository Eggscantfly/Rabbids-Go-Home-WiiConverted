#pragma once

#include "decomp/engine/graph_node.h"
#include "libsm64.h"

// The adapter walks SM64's display lists (the records of gbi_libsm64.h) and writes triangles into one of two sinks:
//  - Mario's (SM64MarioGeometryBuffers): positions, normals, a colour per corner and the coordinates of his atlas,
//    exactly what libsm64 always gave;
//  - the objects' (SM64ObjectGeometryBuffers): the same with an alpha, a texture number and a few flags per
//    triangle, and the drawing layer, for everything else in the level.
// The current sink is chosen by the renderer (rendering_graph_node.c) per object.

enum { GFX_SINK_MARIO = 0, GFX_SINK_OBJECTS = 1 };

extern void gfx_adapter_bind_output_buffers( struct SM64MarioGeometryBuffers *outBuffers );
extern void gfx_adapter_bind_object_buffers( struct SM64ObjectGeometryBuffers *outBuffers );
extern void gfx_adapter_select_sink( int sink );
extern int  gfx_adapter_current_sink( void );
extern void gfx_adapter_frame_begin( void );     // the RDP state (lighting on, plain colours) at the top of a frame
extern void gfx_adapter_set_layer( int layer );  // the master list's drawing layer of what follows
extern void gfx_adapter_set_object( int index ); // which object the triangles that follow belong to
extern void gfx_adapter_run( Mtx *transform, const void *displayList );
