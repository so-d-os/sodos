/* Servico HTTP na porta 8797. GET, POST, PUT, DELETE e HEAD.
 * Sobe no boot. Nao bloqueia o prompt: anda no poll da rede. */
#pragma once

void httpd_start(int port);
void httpd_poll(void);
