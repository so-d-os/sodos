/* ffperfil.h - perfil e cache do Firefox entre boots (fs/ffperfil.c). */
#pragma once
void ffp_win_start(void);
int ffp_poll(void);
int ffp_pct(void);
int ffp_busy(void);
void ffp_finish(void);
void ffp_save(void (*show)(int pct));
/* Um programa terminou sozinho (fim do mod_exec). */
void ffp_program_exit(const char* name, int rc);
