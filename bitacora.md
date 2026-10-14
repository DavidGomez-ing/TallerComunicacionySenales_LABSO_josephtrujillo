# Bitácora — Taller: señales en un servidor concurrente

**Universidad del Cauca — Laboratorio de Sistemas Operativos**

**Estudiante:** Joseph David Trujillo Gomez

**Fecha de entrega:** 14 de octubre de 2026

**Archivo modificado:** `src/servidor_tablero.c` (compila sin advertencias con `gcc -Wall -Wextra`)
**Entorno de las pruebas:** Debian Linux, gcc 13.3.0, bash. Los PIDs y los tiempos cambian entre ejecuciones.

---

## Resumen de lo implementado (puntos 2, 3 y 5)

| Punto | Qué se hizo en `servidor_tablero.c` |
|---|---|
| 2 | Bandera `static volatile sig_atomic_t terminar` y manejador `al_interrumpir`, que solo asigna `terminar = 1`. Se instala en `main` con `sigaction`, antes de aceptar la primera conexión, con `sa_flags = 0` (sin `SA_RESTART`). |
| 3 | El ciclo pasó de `while (1)` a `while (!terminar)`. Si `accept` retorna -1 con `errno == EINTR` no se informa error: el ciclo vuelve a revisar las banderas. Al salir se cancela la alarma, se cierra el socket de escucha y se imprimen las lecturas y escrituras atendidas. |
| 5 | Segunda bandera `alarma_vencida` y manejador `al_vencer_alarma` para `SIGALRM`, también con `sa_flags = 0`. Se programa `alarm(INTERVALO_INFORME)` (5 s) antes del ciclo. Cuando el ciclo observa la bandera, la apaga, llama a `informar_tablero()` y vuelve a programar la alarma. |

`informar_tablero()` usa `leer_tablero()`, la misma función que atiende la orden `LEER`, de modo que aplica la política vigente en modo de lectura. Los manejadores no llaman a `printf` ni toman candados: todo el trabajo ocurre en el ciclo principal.

---

## Punto 1 — Servidor sin modificaciones

Se compiló el servidor original, se arrancó con `./servidor_tablero rwlock`, se conectó `./cliente_tablero 127.0.0.1 lector x 1000` y, con el cliente en pleno trabajo, se interrumpió el servidor con SIGINT (la señal que genera Ctrl-C).

Salida del servidor:

```
Tablero de 64 casillas, politica rwlock, escuchando en el puerto 5500
```

Salida del cliente:

```
La conexion se interrumpio en la repeticion 252
251 lecturas, 0 incoherentes, 14.336 segundos
```

**Qué presenta el servidor al terminar:** nada. Solo aparece la línea de arranque, sin contadores ni mensaje de cierre. El proceso terminó por la acción por defecto de SIGINT (código de salida de señal 2) y el ciclo `while (1)` nunca llegó a las líneas finales (`close(socket_escucha)`), que en la práctica son inalcanzables.

**Qué ocurre con el cliente:** su conexión se corta a mitad de la serie. Al desaparecer el proceso, el kernel cierra todos sus descriptores, incluido el socket de ese cliente. En el cliente, `enviar_orden` deja de recibir la respuesta y retorna 0, por lo que informa que la conexión se interrumpió en la repetición 103 (había completado 102 lecturas). El cliente no recibe ningún aviso del servidor: simplemente se queda sin respuesta.

---

## Punto 4 — Protección de los contadores

**Mecanismo elegido:** enteros atómicos de C11 (`<stdatomic.h>`).

```c
static atomic_long lecturas_atendidas = 0;
static atomic_long escrituras_atendidas = 0;
...
atomic_fetch_add(&lecturas_atendidas, 1);   /* en atender_cliente, tras despachar LEER */
atomic_fetch_add(&escrituras_atendidas, 1); /* en atender_cliente, tras despachar ESCRIBIR */
...
atomic_load(&lecturas_atendidas)            /* al presentar los contadores */
```

**Criterio:**

1. **El dato es un solo entero por contador.** No hay una invariante que relacione los dos contadores entre sí ni con el tablero, así que lo único que se necesita es que cada incremento sea indivisible. `atomic_fetch_add` lo garantiza sin una sección crítica de varias instrucciones.
2. **No se reutiliza el candado del tablero.** Con `rwlock`, las lecturas entran varias a la vez bajo `rdlock`, de modo que incrementar un contador dentro de esa sección seguiría siendo una carrera entre lectores. Con la política `sin` ni siquiera existe candado. Y tomar el candado del tablero solo para contar obligaría a los hilos a esperarse, justo lo que `rwlock` evita.
3. **Un `mutex` propio también sería correcto**, pero añade bloqueo y espera para proteger una sola suma. El atómico no bloquea, no puede producir un bloqueo mutuo y mantiene la protección de los contadores independiente de la política elegida para el tablero.
4. **Se cuenta al despachar la orden**, como pide el enunciado, de modo que una orden cuya respuesta no pudo entregarse porque el cliente ya cerró también queda contada.

**Verificación:** con el servidor en `rwlock` se ejecutó `./carga.sh` (8 lectores × 20 lecturas y 2 escritores × 10 escrituras) y luego SIGINT. Se esperaban 160 lecturas y 20 escrituras:

```
Servidor terminado. Lecturas atendidas: 160, escrituras atendidas: 20
```

Los totales son exactos. (El informe periódico no incrementa los contadores, porque no es una orden de un cliente.)

---

## Punto 5 — Informe periódico

Se arrancó el servidor y se dejó correr. A los ~6,5 s se ejecutó un escritor (`./cliente_tablero 127.0.0.1 escritor A 2`) para ver cambiar la versión del tablero.

```
Tablero de 64 casillas, politica rwlock, escuchando en el puerto 5500
[informe] version 0, tablero: ................................................................
[informe] version 2, tablero: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
[informe] version 2, tablero: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
```

El primer informe sale a los 5 s (versión 0, tablero vacío), el segundo a los 10 s (versión 2, tras las dos escrituras) y el tercero lo provocó a mano `kill -ALRM` (punto 6). La alarma se reprograma después de cada informe, por lo que los informes siguen saliendo cada 5 s.

---

## Punto 6 — Envío de señales con `kill`

Se siguió la secuencia del enunciado desde otra terminal:

```
./servidor_tablero rwlock &
kill -ALRM $!
kill -INT $!
kill -KILL $!      # en una ejecución aparte, porque INT ya terminó el servidor
```

**Efecto de cada señal:**

| Señal | Efecto observado |
|---|---|
| `SIGALRM` | El servidor presenta de inmediato un informe (versión y contenido del tablero) y reprograma la alarma. Sigue vivo. |
| `SIGINT` | El servidor sale del ciclo, cierra el socket de escucha, imprime `Servidor terminado. Lecturas atendidas: N, escrituras atendidas: M` y termina con código de salida 0. |
| `SIGKILL` | El servidor desaparece al instante, con código de salida 137 (128 + 9). No imprime contadores ni ningún mensaje: el manejador no se ejecuta porque no existe. Solo se vio la línea de arranque. |

**Intento de capturar SIGKILL.** Se escribió `prueba_sigkill.c`, que llama a `sigaction` para SIGINT, SIGKILL y SIGSTOP e informa el resultado con `perror`:

```
$ gcc -Wall -Wextra -o prueba_sigkill prueba_sigkill.c
$ ./prueba_sigkill
sigaction(SIGINT) tuvo exito
sigaction(SIGKILL) fallo: sigaction: Invalid argument
sigaction(SIGSTOP) fallo: sigaction: Invalid argument
```

**Por qué el sistema no lo permite:** `SIGKILL` (y `SIGSTOP`) son las dos únicas señales cuya acción el kernel ejecuta sin consultar al proceso. No se pueden capturar, bloquear ni ignorar, y por eso `sigaction` retorna -1 con `EINVAL`.

**En qué beneficia esa negativa:** garantiza que siempre exista una forma de terminar un proceso. Si fuera posible capturar `SIGKILL`, un programa con un manejador defectuoso (o malicioso) que ignore todas las señales, o un proceso colgado dentro de su propio manejador, no podría ser detenido ni siquiera por el administrador, y el sistema no podría recuperar sus recursos. La contrapartida se vio en el experimento: con `SIGKILL` el servidor no hace limpieza ni presenta contadores, por lo que se usa solo como último recurso, después de `SIGINT` o `SIGTERM`. (`SIGSTOP` cumple el papel equivalente para suspender un proceso.)

---

## Punto 7 — Valor de retorno de las llamadas y uso de `perror`

Llamadas agregadas y cómo se informa cada fallo:

| Llamada | Comprobación |
|---|---|
| `sigaction(SIGINT)` y `sigaction(SIGALRM)` | Si retorna -1: `perror("sigaction(SIGINT)")` / `perror("sigaction(SIGALRM)")` y salida con `EXIT_FAILURE`. |
| `sigemptyset` / `sigaddset` | Si retornan -1: `perror("sigaddset")` y salida con `EXIT_FAILURE`. |
| `accept` | Si retorna -1 y `errno == EINTR`, no es un error: el ciclo revisa las banderas. Cualquier otro valor de `errno` se informa con `perror("accept")`. |
| `pthread_sigmask` | Retorna el código de error en lugar de dejarlo en `errno`, así que se informa con `strerror` (en este caso `perror` imprimiría un mensaje equivocado). |
| `close(socket_escucha)` al terminar | Si retorna -1: `perror("close")`. |
| `alarm` | No tiene valor de error: retorna los segundos que faltaban de una alarma anterior, y no puede fallar. |

También se corrigió una llamada ya existente por la misma razón: `pthread_create` retorna el código de error y no lo deja en `errno`, por lo que su mensaje pasó de `perror` a `strerror`. A `pthread_detach` se le añadió la misma comprobación.

**Fallo provocado a propósito:** se arrancó un servidor y, mientras escuchaba, un segundo servidor en el mismo puerto:

```
$ ./servidor_tablero rwlock        # primera terminal, escuchando
$ ./servidor_tablero rwlock        # segunda terminal
bind: Address already in use
$ echo $?
1
```

El segundo servidor crea su socket sin problema, pero `bind` falla con `EADDRINUSE`. `SO_REUSEADDR` solo permite reutilizar un puerto que tiene conexiones antiguas en `TIME_WAIT`; no permite que dos sockets estén escuchando a la vez en el mismo puerto. `crear_socket_escucha` informa el error con `perror`, cierra el socket y retorna -1, y `main` termina con `EXIT_FAILURE`. El primer servidor no se ve afectado.

---

## Punto 8 — `signal(SIGPIPE, SIG_IGN)`

**Efecto de la línea:** cuando un proceso escribe en un socket cuyo otro extremo ya cerró la conexión, el kernel le envía `SIGPIPE`, y la acción por defecto de esa señal es terminar el proceso. Con `SIG_IGN` la señal se descarta y `write` retorna -1 con `errno == EPIPE`. En `atender_cliente` eso se traduce en el `break` posterior a `if (write(...) == -1)`: termina solo el hilo de ese cliente, que cierra su socket, y el resto del servidor sigue funcionando. La disposición `SIG_IGN` se aplica a todo el proceso, así que cubre a todos los hilos.

**Qué le ocurriría al servidor sin ella:** el cierre brusco de un solo cliente mataría el servidor completo, con todos los demás clientes conectados.

**Comprobación:** se construyó una variante de prueba (no se entrega) sin esa línea. Un cliente envió dos órdenes `LEER` seguidas y cerró la conexión sin leer ninguna respuesta. La primera respuesta se escribe sin problema, el cliente cerrado contesta con un RST y la escritura siguiente produce `EPIPE`:

```
Servidor entregado (con la línea):
  servidor vivo tras el cierre del cliente
  respuesta a un segundo cliente: OK 1

Variante sin la línea (4 de 4 ejecuciones):
  servidor TERMINADO por el cierre del cliente; codigo de salida: -13 => senal 13
```

La señal 13 es `SIGPIPE`: sin la línea el servidor murió las cuatro veces, y con ella siguió atendiendo a otro cliente.

---

## Decisiones adicionales y limitaciones

- **Hilos y señales.** Una señal dirigida al proceso puede ser entregada a cualquier hilo que no la tenga bloqueada. Si llegara a un hilo de cliente, su manejador activaría la bandera, pero `accept` (en el hilo principal) no se interrumpiría. Por eso `main` bloquea `SIGINT` y `SIGALRM` justo antes de `pthread_create` y restaura su máscara justo después: los hilos heredan la máscara bloqueada y la señal siempre llega al hilo principal.
- **Limitación conocida.** Existe una ventana muy corta entre la comprobación `while (!terminar)` y la llamada a `accept`. Si una señal cae exactamente ahí, `accept` no se interrumpe y el ciclo no observa la bandera hasta que llegue una conexión o otra señal. Con `SIGINT` basta repetir Ctrl-C. Cerrar la ventana por completo exige `pselect`/`ppoll` o una tubería auxiliar, que quedan fuera del alcance de este taller y de lo que pide el enunciado.

---

## Archivos de la entrega

```
Comunicacion_Sincronizacion_Senales/
├── bitacora.md
├── Comunicacion_Sincronizacion_Senales.pdf
└── src/
    ├── Makefile
    ├── carga.sh
    ├── cliente_tablero.c
    ├── servidor_tablero.c      (modificado)
    └── prueba_sigkill.c        (programa de prueba del punto 6)
```

Compilación: `cd src && make` (usa `-Wall -Wextra -O2`, sin advertencias). `prueba_sigkill.c` se compila aparte con `gcc -Wall -Wextra -o prueba_sigkill prueba_sigkill.c`.
