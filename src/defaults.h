#pragma once

#define DO_EXPAND(VAL) VAL ## 1
#define EXPAND(VAL) DO_EXPAND(VAL)

#define DO_QUOTE(X) #X
#define QUOTE(X) DO_QUOTE(X)

#ifndef DEFAULT_NAME
#define DEFAULT_NAME "X1 Bridge"
#endif

#ifndef DEFAULT_PIN
#define DEFAULT_PIN 123456
#endif

#if (defined OTA_PUBLIC_KEY_X) && (EXPAND(OTA_PUBLIC_KEY_X) == 1)
#undef OTA_PUBLIC_KEY_X
#endif

#if (defined OTA_PUBLIC_KEY_Y) && (EXPAND(OTA_PUBLIC_KEY_Y) == 1)
#undef OTA_PUBLIC_KEY_Y
#endif

#undef EXPAND
#undef DO_EXPAND
