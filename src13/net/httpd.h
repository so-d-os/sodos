/* Servico HTTP na porta 8797. GET, POST, PUT, DELETE e HEAD.
 * Sobe no boot quando ha rede; api start/stop controla depois.
 * Nao bloqueia o prompt: anda no poll da rede. */
#pragma once

void httpd_start(int port);
/* Para o servico e derruba a conexao em andamento. */
void httpd_stop(void);
void httpd_poll(void);
/* Porta em que o servico escuta, 0 se nao subiu. */
int httpd_port(void);
