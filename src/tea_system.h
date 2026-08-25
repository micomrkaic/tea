/* tea_system.h — validated native subprocess execution. */
#ifndef TEA_SYSTEM_H
#define TEA_SYSTEM_H

#ifndef __EMSCRIPTEN__
int tea_system(const char *command);
#define system tea_system
#endif

#endif
