/*
 * sqNamedPrims.h - iOS Pharo VM Plugin Exports
 *
 * This file registers built-in plugins for the iOS VM.
 * All plugins compiled with SQUEAK_BUILTIN_PLUGIN are registered here.
 */

/* Core VM exports */
extern sqExport vm_exports[];
extern sqExport os_exports[];

/* Built-in plugin exports - essential for iOS */
extern void* BitBltPlugin_exports[][3];
extern void* B2DPlugin_exports[][3];
extern void* MiscPrimitivePlugin_exports[][3];
extern void* LargeIntegers_exports[][3];
extern void* FileAttributesPlugin_exports[][3];
extern void* LocalePlugin_exports[][3];
extern void* DSAPrims_exports[][3];

/* Cast to sqExport* for the array */
sqExport *pluginExports[] = {
    vm_exports,
    os_exports,
    (sqExport*)BitBltPlugin_exports,      /* Essential: display/graphics */
    (sqExport*)B2DPlugin_exports,         /* 2D graphics/Balloon */
    (sqExport*)MiscPrimitivePlugin_exports, /* Common primitives */
    (sqExport*)LargeIntegers_exports,     /* Large integer math */
    (sqExport*)FileAttributesPlugin_exports, /* File operations */
    (sqExport*)LocalePlugin_exports,      /* Locale support */
    (sqExport*)DSAPrims_exports,          /* DSA cryptography */
    NULL
};
