/*
 * Readiness bits, POSIX poll(2) numbering.
 *
 * Lives on its own because two layers need the same numbers and neither may
 * include the other: the VFS reports readiness of a descriptor, the backend
 * reports readiness of a transport. If these lived in the backend header, fd.h
 * would have to include the backend interface - which is exactly the dependency
 * the boundary forbids.
 */

#ifndef OMI_READINESS_H
#define OMI_READINESS_H

#define OMI_POLLIN  0x001
#define OMI_POLLOUT 0x004
#define OMI_POLLERR 0x008
#define OMI_POLLHUP 0x010

#endif /* OMI_READINESS_H */
