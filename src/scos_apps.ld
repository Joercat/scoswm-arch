/* App-registry section, the Linux equivalent of the block in SCos's
 * kernel/x86_64/linker.ld: built-in apps self-register with
 * SCOS_APP(symbol, order) into .scos_apps.<order>, and apps.c walks
 * __scos_apps_start..__scos_apps_end.  GNU ld (and lld) accept this
 * INSERT fragment alongside the default script. */
SECTIONS {
    .scos_apps : {
        __scos_apps_start = .;
        KEEP(*(SORT_BY_NAME(.scos_apps.*)))
        __scos_apps_end = .;
    }
} INSERT AFTER .data.rel.ro;
