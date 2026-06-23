/* miniaudio_impl.c - The single translation unit that compiles miniaudio.
 *
 * miniaudio ships as one big header; its implementation must be emitted in
 * exactly one source file. Keeping it isolated here means the rest of the GUI
 * only sees the declarations and compiles fast. We disable a few subsystems we
 * never use to keep the binary lean. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#include "miniaudio.h"
