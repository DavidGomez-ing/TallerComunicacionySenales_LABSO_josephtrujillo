/**
 * @file
 * @brief Servidor TCP concurrente con un recurso compartido entre lectores y escritores.
 *
 * El servidor mantiene un tablero de texto en memoria. Atiende cada conexion en un hilo
 * independiente, de modo que varios clientes operan sobre el tablero al mismo tiempo. La
 * politica de sincronizacion se elige al arrancar:
 *
 *   sin     - ninguna proteccion sobre el tablero
 *   mutex   - exclusion mutua total: un solo cliente a la vez, lea o escriba
 *   rwlock  - lectores y escritores: varias lecturas simultaneas, escritura exclusiva
 *
 * Protocolo de texto, una orden por linea:
 *
 *   LEER              -> OK <version> <contenido del tablero>
 *   ESCRIBIR <car>    -> OK <version>
 *   SALIR             -> cierra la conexion
 *
 * Senales:
 *
 *   SIGINT  - el servidor termina de forma ordenada: cierra el socket de escucha y
 *             presenta cuantas lecturas y escrituras atendio.
 *   SIGALRM - alarma periodica: el servidor presenta la version y el contenido del tablero
 *             y programa la alarma de nuevo.
 *
 * Los manejadores solo registran el hecho en una bandera; el trabajo ocurre en el ciclo
 * principal.
 *
 * @author Erwin Meza Vega <emezav@unicauca.edu.co>
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/** @brief Puerto en el que el servidor recibe conexiones. */
#define PUERTO 5500

/** @brief Numero de casillas del tablero compartido. */
#define TAM_TABLERO 64

/** @brief Tamano del buffer de ordenes y respuestas. */
#define TAM_BUFFER 256

/** @brief Microsegundos de espera entre casilla y casilla al escribir. */
#define RETARDO_ESCRITURA 2000

/** @brief Microsegundos de espera entre casilla y casilla al leer. */
#define RETARDO_LECTURA 200

/** @brief Segundos entre dos informes periodicos del estado del tablero. */
#define INTERVALO_INFORME 5

/** @brief Politicas de sincronizacion disponibles. */
typedef enum {
    SIN_PROTECCION, /**< El tablero queda expuesto a accesos simultaneos. */
    CON_MUTEX,      /**< Un solo cliente accede al tablero, lea o escriba. */
    CON_RWLOCK      /**< Varios lectores a la vez, un escritor en exclusiva. */
} politica;

/* Recurso compartido: todas las casillas del tablero contienen el mismo caracter */
static char tablero[TAM_TABLERO];

/* Numero de la ultima escritura completada */
static int version_tablero;

/* Mecanismos de sincronizacion sobre el tablero */
static pthread_mutex_t candado = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t candado_rw = PTHREAD_RWLOCK_INITIALIZER;

/* Politica en uso durante esta ejecucion */
static politica politica_actual = SIN_PROTECCION;

/*
 * Contadores de operaciones atendidas. Los incrementan los hilos que atienden a los clientes
 * y los lee el ciclo principal, de modo que son memoria compartida. Son enteros atomicos:
 * cada incremento es una sola operacion indivisible, sin candado y sin que los hilos se
 * esperen entre si.
 */
static atomic_long lecturas_atendidas = 0;
static atomic_long escrituras_atendidas = 0;

/*
 * Banderas que los manejadores de senales activan y el ciclo principal observa.
 * volatile sig_atomic_t es el unico tipo cuyo acceso desde un manejador esta garantizado.
 */
static volatile sig_atomic_t terminar = 0;
static volatile sig_atomic_t alarma_vencida = 0;

/**
 * @brief Manejador de SIGINT: solo registra que se pidio terminar.
 * @param senal Numero de la senal recibida.
 */
static void al_interrumpir(int senal) {
    (void) senal;
    terminar = 1;
}

/**
 * @brief Manejador de SIGALRM: solo registra que se cumplio el intervalo del informe.
 * @param senal Numero de la senal recibida.
 */
static void al_vencer_alarma(int senal) {
    (void) senal;
    alarma_vencida = 1;
}

/**
 * @brief Traduce el nombre de la politica recibido por linea de comandos.
 * @param nombre Cadena con el nombre de la politica.
 * @param destino Apuntador a la variable que recibe la politica reconocida.
 * @return 1 si el nombre corresponde a una politica conocida, 0 en caso contrario.
 */
int reconocer_politica(const char *nombre, politica *destino);

/**
 * @brief Crea el socket de escucha del servidor.
 * @param puerto Puerto en el que se reciben las conexiones.
 * @return Descriptor del socket de escucha, o -1 si ocurre un error.
 */
int crear_socket_escucha(int puerto);

/**
 * @brief Atiende las ordenes de un cliente hasta que cierra la conexion.
 * @param arg Apuntador al descriptor del socket del cliente, reservado por el llamador.
 * @return NULL siempre.
 */
void *atender_cliente(void *arg);

/**
 * @brief Lee una linea terminada en salto de linea desde un socket.
 * @param socket_cliente Descriptor del socket.
 * @param destino Buffer que recibe la linea, sin el salto de linea final.
 * @param tam Tamano del buffer de destino.
 * @return Numero de caracteres leidos, o -1 si el cliente cerro la conexion o hubo error.
 */
int leer_linea(int socket_cliente, char *destino, int tam);

/**
 * @brief Copia el contenido del tablero aplicando la politica de sincronizacion vigente.
 * @param destino Buffer que recibe la copia, con al menos TAM_TABLERO + 1 caracteres.
 * @return Version del tablero observada durante la lectura.
 */
int leer_tablero(char *destino);

/**
 * @brief Llena el tablero con un caracter aplicando la politica de sincronizacion vigente.
 * @param caracter Caracter que se escribe en todas las casillas.
 * @return Version del tablero despues de la escritura.
 */
int escribir_tablero(char caracter);

/**
 * @brief Presenta la version y el contenido del tablero.
 *
 * La lectura aplica la politica vigente en modo de lectura, igual que la orden LEER.
 */
void informar_tablero(void);

/**
 * @brief Cambia la mascara de senales del hilo que llama e informa el error si ocurre.
 * @param como SIG_BLOCK, SIG_UNBLOCK o SIG_SETMASK.
 * @param nuevo Conjunto de senales que se aplica.
 * @param anterior Si no es NULL, recibe la mascara que estaba vigente.
 * @return 0 si el cambio se realizo, -1 en caso contrario.
 */
int cambiar_mascara(int como, const sigset_t *nuevo, sigset_t *anterior);

int main(int argc, char *argv[]) {
    int socket_escucha;
    int socket_cliente;
    int *descriptor;
    int creado;
    int detenido;
    int restaurada;
    pthread_t hilo;
    struct sigaction accion;
    sigset_t senales_servidor;
    sigset_t mascara_anterior;

    if (argc != 2 || !reconocer_politica(argv[1], &politica_actual)) {
        fprintf(stderr, "Uso: %s sin|mutex|rwlock\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Un cliente que cierra de golpe no debe terminar el servidor */
    signal(SIGPIPE, SIG_IGN);

    memset(tablero, '.', TAM_TABLERO);
    version_tablero = 0;

    /*
     * Manejadores de SIGINT y SIGALRM, instalados antes de aceptar la primera conexion.
     * sa_flags queda en cero de forma deliberada: sin SA_RESTART, accept retorna -1 con
     * errno EINTR cuando llega una senal, y el ciclo principal recupera el control.
     */
    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = al_interrumpir;
    sigemptyset(&accion.sa_mask);
    accion.sa_flags = 0;

    if (sigaction(SIGINT, &accion, NULL) == -1) {
        perror("sigaction(SIGINT)");
        return EXIT_FAILURE;
    }

    accion.sa_handler = al_vencer_alarma;

    if (sigaction(SIGALRM, &accion, NULL) == -1) {
        perror("sigaction(SIGALRM)");
        return EXIT_FAILURE;
    }

    /* Conjunto que se bloquea al crear cada hilo, para que las senales lleguen al principal */
    if (sigemptyset(&senales_servidor) == -1
        || sigaddset(&senales_servidor, SIGINT) == -1
        || sigaddset(&senales_servidor, SIGALRM) == -1) {
        perror("sigaddset");
        return EXIT_FAILURE;
    }

    socket_escucha = crear_socket_escucha(PUERTO);
    if (socket_escucha == -1) {
        return EXIT_FAILURE;
    }

    printf("Tablero de %d casillas, politica %s, escuchando en el puerto %d\n",
           TAM_TABLERO, argv[1], PUERTO);
    fflush(stdout);

    /* Primera alarma: las siguientes las programa el ciclo despues de cada informe */
    alarm(INTERVALO_INFORME);

    while (!terminar) {
        /* El manejador solo activa la bandera; el informe se presenta aqui */
        if (alarma_vencida) {
            alarma_vencida = 0;
            informar_tablero();
            alarm(INTERVALO_INFORME);
        }

        socket_cliente = accept(socket_escucha, NULL, NULL);
        if (socket_cliente == -1) {
            /* EINTR no es un error: una senal interrumpio accept y el ciclo revisa las banderas */
            if (errno != EINTR) {
                perror("accept");
            }
            continue;
        }

        /* El hilo recibe su propia copia del descriptor y la libera al terminar */
        descriptor = malloc(sizeof(int));
        if (descriptor == NULL) {
            close(socket_cliente);
            continue;
        }
        *descriptor = socket_cliente;

        /*
         * El hilo hereda la mascara de senales de quien lo crea. Con SIGINT y SIGALRM
         * bloqueadas durante la creacion, los hilos de los clientes nunca las reciben y
         * el sistema las entrega siempre al hilo principal, que es el que espera en accept.
         */
        if (cambiar_mascara(SIG_BLOCK, &senales_servidor, &mascara_anterior) == -1) {
            free(descriptor);
            close(socket_cliente);
            continue;
        }

        creado = pthread_create(&hilo, NULL, atender_cliente, descriptor);
        restaurada = cambiar_mascara(SIG_SETMASK, &mascara_anterior, NULL);

        if (creado != 0) {
            /* pthread_create devuelve el codigo de error, no lo deja en errno */
            fprintf(stderr, "pthread_create: %s\n", strerror(creado));
            free(descriptor);
            close(socket_cliente);
        } else {
            /* Nadie espera el resultado del hilo: sus recursos se liberan al terminar */
            detenido = pthread_detach(hilo);
            if (detenido != 0) {
                fprintf(stderr, "pthread_detach: %s\n", strerror(detenido));
            }
        }

        /* Sin recuperar la mascara el hilo principal no recibiria senales: se termina */
        if (restaurada == -1) {
            break;
        }
    }

    /* Cierre ordenado: se cancela la alarma, se cierra el socket y se presentan los contadores */
    alarm(0);

    if (close(socket_escucha) == -1) {
        perror("close");
    }

    printf("Servidor terminado. Lecturas atendidas: %ld, escrituras atendidas: %ld\n",
           atomic_load(&lecturas_atendidas), atomic_load(&escrituras_atendidas));
    fflush(stdout);

    return terminar ? EXIT_SUCCESS : EXIT_FAILURE;
}

int reconocer_politica(const char *nombre, politica *destino) {
    if (strcmp(nombre, "sin") == 0) {
        *destino = SIN_PROTECCION;
        return 1;
    }
    if (strcmp(nombre, "mutex") == 0) {
        *destino = CON_MUTEX;
        return 1;
    }
    if (strcmp(nombre, "rwlock") == 0) {
        *destino = CON_RWLOCK;
        return 1;
    }
    return 0;
}

int crear_socket_escucha(int puerto) {
    int socket_escucha;
    int reutilizar = 1;
    struct sockaddr_in direccion;

    socket_escucha = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_escucha == -1) {
        perror("socket");
        return -1;
    }

    /* El puerto queda disponible de inmediato al terminar el servidor */
    setsockopt(socket_escucha, SOL_SOCKET, SO_REUSEADDR, &reutilizar, sizeof(reutilizar));

    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_addr.s_addr = htonl(INADDR_ANY);
    direccion.sin_port = htons(puerto);

    if (bind(socket_escucha, (struct sockaddr *) &direccion, sizeof(direccion)) == -1) {
        perror("bind");
        close(socket_escucha);
        return -1;
    }

    if (listen(socket_escucha, 16) == -1) {
        perror("listen");
        close(socket_escucha);
        return -1;
    }

    return socket_escucha;
}

void *atender_cliente(void *arg) {
    int socket_cliente = *((int *) arg);
    char orden[TAM_BUFFER];
    char copia[TAM_TABLERO + 1];
    char respuesta[TAM_BUFFER];
    int version;
    int longitud;

    free(arg);

    while (1) {
        longitud = leer_linea(socket_cliente, orden, TAM_BUFFER);
        if (longitud == -1) {
            break;
        }

        if (strcmp(orden, "LEER") == 0) {
            version = leer_tablero(copia);
            atomic_fetch_add(&lecturas_atendidas, 1);
            snprintf(respuesta, TAM_BUFFER, "OK %d %s\n", version, copia);
        } else if (strncmp(orden, "ESCRIBIR ", 9) == 0 && orden[9] != '\0') {
            version = escribir_tablero(orden[9]);
            atomic_fetch_add(&escrituras_atendidas, 1);
            snprintf(respuesta, TAM_BUFFER, "OK %d\n", version);
        } else if (strcmp(orden, "SALIR") == 0) {
            break;
        } else {
            snprintf(respuesta, TAM_BUFFER, "ERROR orden no reconocida\n");
        }

        if (write(socket_cliente, respuesta, strlen(respuesta)) == -1) {
            break;
        }
    }

    close(socket_cliente);
    return NULL;
}

int leer_linea(int socket_cliente, char *destino, int tam) {
    int total = 0;
    char caracter;
    ssize_t leidos;

    while (total < tam - 1) {
        leidos = read(socket_cliente, &caracter, 1);

        /* El cliente cerro la conexion: una linea a medias no se procesa */
        if (leidos <= 0) {
            return -1;
        }
        if (caracter == '\n') {
            break;
        }
        if (caracter != '\r') {
            destino[total] = caracter;
            total++;
        }
    }

    destino[total] = '\0';
    return total;
}

int leer_tablero(char *destino) {
    int i;
    int version;

    if (politica_actual == CON_MUTEX) {
        pthread_mutex_lock(&candado);
    } else if (politica_actual == CON_RWLOCK) {
        pthread_rwlock_rdlock(&candado_rw);
    }

    /* La copia casilla por casilla amplia la ventana en la que otro hilo puede intervenir */
    for (i = 0; i < TAM_TABLERO; i++) {
        destino[i] = tablero[i];
        usleep(RETARDO_LECTURA);
    }
    destino[TAM_TABLERO] = '\0';
    version = version_tablero;

    if (politica_actual == CON_MUTEX) {
        pthread_mutex_unlock(&candado);
    } else if (politica_actual == CON_RWLOCK) {
        pthread_rwlock_unlock(&candado_rw);
    }

    return version;
}

int escribir_tablero(char caracter) {
    int i;
    int version;

    if (politica_actual == CON_MUTEX) {
        pthread_mutex_lock(&candado);
    } else if (politica_actual == CON_RWLOCK) {
        pthread_rwlock_wrlock(&candado_rw);
    }

    for (i = 0; i < TAM_TABLERO; i++) {
        tablero[i] = caracter;
        usleep(RETARDO_ESCRITURA);
    }
    version_tablero++;
    version = version_tablero;

    if (politica_actual == CON_MUTEX) {
        pthread_mutex_unlock(&candado);
    } else if (politica_actual == CON_RWLOCK) {
        pthread_rwlock_unlock(&candado_rw);
    }

    return version;
}

void informar_tablero(void) {
    char copia[TAM_TABLERO + 1];
    int version;

    /* Misma lectura que atiende la orden LEER: aplica la politica vigente en modo lectura */
    version = leer_tablero(copia);

    printf("[informe] version %d, tablero: %s\n", version, copia);
    fflush(stdout);
}

int cambiar_mascara(int como, const sigset_t *nuevo, sigset_t *anterior) {
    int error;

    error = pthread_sigmask(como, nuevo, anterior);
    if (error != 0) {
        /* Las funciones de pthreads devuelven el codigo de error, no lo dejan en errno */
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        return -1;
    }

    return 0;
}
