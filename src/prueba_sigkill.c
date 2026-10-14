/**
 * @file
 * @brief Intenta instalar un manejador para SIGKILL y para SIGSTOP (punto 6 del taller).
 *
 * El sistema operativo no permite capturar, bloquear ni ignorar estas dos senales: sigaction
 * retorna -1 y deja EINVAL en errno. El programa informa cada resultado con perror.
 *
 * Compilacion: gcc -Wall -Wextra -o prueba_sigkill prueba_sigkill.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/**
 * @brief Manejador de prueba: nunca llega a ejecutarse para SIGKILL ni SIGSTOP.
 * @param senal Numero de la senal recibida.
 */
static void manejador(int senal) {
    (void) senal;
}

/**
 * @brief Intenta instalar el manejador de prueba para una senal e informa el resultado.
 * @param senal Numero de la senal.
 * @param nombre Nombre de la senal, para los mensajes.
 */
static void intentar_capturar(int senal, const char *nombre) {
    struct sigaction accion;

    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = manejador;
    sigemptyset(&accion.sa_mask);
    accion.sa_flags = 0;

    if (sigaction(senal, &accion, NULL) == -1) {
        printf("sigaction(%s) fallo: ", nombre);
        fflush(stdout);
        perror("sigaction");
    } else {
        printf("sigaction(%s) tuvo exito\n", nombre);
    }
}

int main(void) {
    intentar_capturar(SIGINT, "SIGINT");
    intentar_capturar(SIGKILL, "SIGKILL");
    intentar_capturar(SIGSTOP, "SIGSTOP");

    return EXIT_SUCCESS;
}
