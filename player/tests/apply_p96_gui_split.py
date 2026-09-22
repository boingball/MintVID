#!/usr/bin/env python3
"""One-shot source migration for the P96 startup selector; removed before PR."""
from pathlib import Path


def change(file, old, new, count=1):
    path = Path(file)
    source = path.read_text()
    actual = source.count(old)
    if actual != count:
        raise RuntimeError(f'{file}: expected {count} occurrences of {old!r}, found {actual}')
    path.write_text(source.replace(old, new))


h = 'player/core/mr_play_options.h'
change(h, '    MR_DISPLAY_AGA_EHB\n} mr_display_mode;',
          '    MR_DISPLAY_AGA_EHB,\n    /* Fullscreen P96 startup; append to preserve existing saved enum values. */\n    MR_DISPLAY_P96_FULLSCREEN\n} mr_display_mode;')

c = 'player/core/mr_play_options.c'
change(c, '    case MR_DISPLAY_P96: return "p96";',
          '    case MR_DISPLAY_P96: return "p96";\n    case MR_DISPLAY_P96_FULLSCREEN: return "p96-fullscreen";')
change(c, 'o->display != MR_DISPLAY_CGX && o->display != MR_DISPLAY_P96)',
          'o->display != MR_DISPLAY_CGX && o->display != MR_DISPLAY_P96 &&\n            o->display != MR_DISPLAY_P96_FULLSCREEN)', 2)
change(c, 'if (o->display == MR_DISPLAY_P96 && !append_option(out, cap, "--p96")) return 0;',
          'if ((o->display == MR_DISPLAY_P96 ||\n             o->display == MR_DISPLAY_P96_FULLSCREEN) &&\n            !append_option(out, cap, "--p96")) return 0;\n        if (o->display == MR_DISPLAY_P96_FULLSCREEN &&\n            !append_option(out, cap, "--fullscreen")) return 0;')
change(c, 'else if (!strcmp(value, "p96")) o->display = MR_DISPLAY_P96;',
          'else if (!strcmp(value, "p96")) o->display = MR_DISPLAY_P96;\n            else if (!strcmp(value, "p96-fullscreen")) o->display = MR_DISPLAY_P96_FULLSCREEN;')
change(c, 'if (o->display == MR_DISPLAY_CGX || o->display == MR_DISPLAY_P96)',
          'if (o->display == MR_DISPLAY_CGX || o->display == MR_DISPLAY_P96 ||\n        o->display == MR_DISPLAY_P96_FULLSCREEN)')
change(c, 'o->display == MR_DISPLAY_P96 ? "P96" : "WritePixel",',
          'o->display == MR_DISPLAY_P96 ? "P96" :\n                 o->display == MR_DISPLAY_P96_FULLSCREEN ? "P96 Fullscreen" :\n                 "WritePixel",')

r = 'player/amiga/mrgui.c'
change(r, 'static mr_display_mode mode_values[8];',
          'static mr_display_mode mode_values[9];')
change(r, 'mode_values[selected] == MR_DISPLAY_P96)',
          'mode_values[selected] == MR_DISPLAY_P96 ||\n                               mode_values[selected] == MR_DISPLAY_P96_FULLSCREEN)')
change(r, '(have_rtg && !add_mode_node(&modes, "RTG (P96)", MR_DISPLAY_P96)))',
          '(have_rtg && !add_mode_node(&modes, "P96 (Windowed)", MR_DISPLAY_P96)) ||\n        (have_rtg && !add_mode_node(&modes, "P96 (Fullscreen)",\n                                    MR_DISPLAY_P96_FULLSCREEN)))')

g = 'player/amiga/mrgui_gadtools.c'
change(g, 'mr_display_mode modes[8];', 'mr_display_mode modes[9];')
change(g, 'STRPTR mode_labels[9];', 'STRPTR mode_labels[10];')
change(g, 'app->modes[selected] == MR_DISPLAY_P96);',
          'app->modes[selected] == MR_DISPLAY_P96 ||\n                      app->modes[selected] == MR_DISPLAY_P96_FULLSCREEN);')
change(g, 'app->mode_labels[app->mode_count] = (STRPTR)"Display: RTG (P96)";\n        app->modes[app->mode_count++] = MR_DISPLAY_P96;',
          'app->mode_labels[app->mode_count] = (STRPTR)"Display: P96 (Windowed)";\n        app->modes[app->mode_count++] = MR_DISPLAY_P96;\n        app->mode_labels[app->mode_count] = (STRPTR)"Display: P96 (Fullscreen)";\n        app->modes[app->mode_count++] = MR_DISPLAY_P96_FULLSCREEN;')

t = 'player/tests/mr_iptv_check.c'
anchor = ('      assert(strstr(summary, "RTG (P96)"));\n'
          '    }\n'
          '    {\n'
          '      char *inherited[] = {"iptvgui", "--display", "ecs32"};')
addition = ('      assert(strstr(summary, "RTG (P96)"));\n'
            '    }\n'
            '    {\n'
            '      /* The fullscreen GUI choice must survive browser inheritance. */\n'
            '      char *inherited[] = {"iptvgui", "--display", "p96-fullscreen"};\n'
            '      char args[4096], summary[160], error[128];\n'
            '      mr_play_options parsed;\n'
            '      mr_play_options_default(&parsed);\n'
            '      assert(mr_play_options_parse(&parsed, 3, inherited, error,\n'
            '                                   sizeof(error)));\n'
            '      assert(parsed.display == MR_DISPLAY_P96_FULLSCREEN);\n'
            '      assert(mr_build_player_arguments(args, sizeof(args), &parsed,\n'
            '                                       launch.url, NULL, NULL));\n'
            '      assert(strstr(args, "--p96 --fullscreen"));\n'
            '      assert(!strstr(args, "--aga") && !strstr(args, "--kalms-c2p"));\n'
            '      mr_play_options_summary(&parsed, summary, sizeof(summary));\n'
            '      assert(strstr(summary, "RTG (P96 Fullscreen)"));\n'
            '      assert(mr_build_iptv_arguments(args, sizeof(args), &parsed));\n'
            '      assert(strstr(args, "--display p96-fullscreen"));\n'
            '      assert(!strstr(args, "--c2p"));\n'
            '    }\n'
            '    {\n'
            '      char *inherited[] = {"iptvgui", "--display", "ecs32"};')
change(t, anchor, addition)
