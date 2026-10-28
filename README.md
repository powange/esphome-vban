# esphome-vban

Composants ESPHome pour diffuser et recevoir du VBAN (audio UDP) sur un ESP32.
Deux composants indépendants : `vban_sender` (micro → réseau) et `vban_receiver`
(réseau → speaker). Compatibles unicast, broadcast et multicast.

## Installation

```yaml
external_components:
  - source: github://powange/esphome-vban
    components: [vban_sender, vban_receiver]
    refresh: 0s
```

## `vban_sender`

Capture le flux PCM 16-bit 16 kHz mono d'un microphone ESPHome et l'émet en
paquets VBAN UDP.

```yaml
vban_sender:
  microphone: atom_mic          # ID d'un composant microphone: ESPHome
  target_ip: "192.168.0.255"    # unicast, broadcast de subnet ou multicast
  target_port: 6980
  stream_name: "AtomBureau"     # 16 caractères max
  gain: 1.0                     # amplification logicielle (0.0–N), optionnel
```

Le composant s'abonne au callback du micro et démarre automatiquement la
capture après le setup. Aucune boucle `interval:` n'est nécessaire.

| Paramètre      | Requis | Défaut     | Description |
|----------------|--------|------------|-------------|
| `microphone`   | ✅     | —          | ID du micro ESPHome |
| `target_ip`    | ✅     | —          | Destination (unicast / broadcast / multicast) |
| `target_port`  | ❌     | `6980`     | Port UDP |
| `stream_name`  | ❌     | `AtomEcho` | Nom du flux VBAN (16 chars max) |
| `gain`         | ❌     | `1.0`      | Gain logiciel avec clipping `±32767` |

## `vban_receiver`

Écoute un port UDP, filtre par stream name, et pousse le PCM reçu vers un
speaker ESPHome. Gère automatiquement l'arrêt/redémarrage du microphone pour
libérer le bus I2S pendant la lecture (le `microphone:` est optionnel — à
fournir uniquement si un `vban_sender` utilise le même bus I2S).

```yaml
vban_receiver:
  speaker: atom_speaker         # ID d'un composant speaker: ESPHome
  microphone: atom_mic          # optionnel: mis en pause pendant la lecture
  listen_port: 6980
  stream_name: "TTS1"           # 16 chars max, doit matcher l'émetteur
  idle_timeout_ms: 1500         # délai sans paquet avant arrêt du speaker
  prebuffer_ms: 200             # tampon anti-gigue avant lecture
```

| Paramètre          | Requis | Défaut | Description |
|--------------------|--------|--------|-------------|
| `speaker`          | ✅     | —      | ID du speaker ESPHome |
| `microphone`       | ❌     | —      | ID du micro à mettre en pause |
| `listen_port`      | ❌     | `6980` | Port UDP d'écoute |
| `stream_name`      | ✅     | —      | Nom du flux à accepter (16 chars max) |
| `idle_timeout_ms`  | ❌     | `1500` | Durée sans paquet avant `stop()` |
| `prebuffer_ms`     | ❌     | `200`  | Son mis en tampon avant de démarrer la lecture (0–1000) |

### Tampon anti-gigue

En Wi-Fi, les paquets arrivent de façon irrégulière (gigue), surtout avec un
signal faible. Tous les paquets passent donc par un tampon circulaire : la
lecture ne démarre qu'une fois `prebuffer_ms` de son accumulé (ou si aucun
paquet n'arrive pendant 150 ms, pour les réponses courtes). Si le tampon se
vide en cours de flux (underrun), le receiver se remet en attente avec un
seuil réduit à `prebuffer_ms / 2`. Augmenter `prebuffer_ms` si le son reste
haché, au prix d'une latence plus élevée.

À la fin de chaque flux, un résumé est logué en `DEBUG` :

```
[vban_rx] Stream 'TTS1' ended: 412 packets, 0 lost, 0 out of order, 0 underruns
```

## Format audio supporté

Les deux composants supportent exclusivement :

- Sample rate : **16000 Hz**
- Format     : **PCM 16-bit signed**
- Channels   : **1 (mono)**

Le receiver rejette avec un warning les paquets d'un autre format.

## Exemple complet

Voir [examples/atom-echos3r.yaml](examples/atom-echos3r.yaml) pour une config
M5Stack Atom Echo S3R avec sender + receiver + media_player Home Assistant.

## Diagnostics

Chaque composant expose son état via `dump_config` (visible au boot ou à la
connexion d'un client API) :

```
VBAN Sender:
  Target: 192.168.0.255:6980
  Stream name: AtomBureau
  Socket: OK
  Mic started: YES
  Frames sent: 3069

VBAN Receiver:
  Listen port: 6980
  Stream name: TTS1
  Socket: OK
  Packets received: 124
  Packets lost:     0
  Out of order:     0
  Underruns:        0
```

## Half-duplex I2S

L'implémentation `i2s_audio` d'ESPHome ne supporte pas le full-duplex sur un
même bus. Sur les plateformes avec un seul jeu de pins vers le codec (Atom
Echo S3R, etc.), le `vban_receiver` arrête le mic pendant la lecture puis le
relance au bout de `idle_timeout_ms` sans paquet.

L'arrêt du micro étant asynchrone, le speaker n'est démarré qu'une fois le
micro effectivement arrêté (sinon `Parent bus is busy`), avec un délai max de
500 ms. De même, le micro n'est relancé qu'une fois le speaker arrêté.

## Recommandations Wi-Fi

- `wifi: power_save_mode: none` : indispensable pour l'audio. Le mode
  d'économie d'énergie par défaut provoque des pertes de paquets et une voix
  accélérée.
- Préférer l'**unicast** (IP du serveur) au broadcast pour `vban_sender` :
  en Wi-Fi, le broadcast est émis au débit minimal et sans accusé de
  réception, donc sans retransmission en cas de perte.
- Surveiller le signal avec un capteur `wifi_signal` : en dessous de
  -70 dBm environ, augmenter `prebuffer_ms`.

## File UDP de lwIP (receiver)

Par défaut, lwIP ne garde que **6 paquets** en attente par socket UDP ; au-delà,
les paquets sont jetés en silence. Comme l'émetteur TTS envoie par rafales, la
file déborde entre deux passages dans `loop()`. Le `vban_receiver` limite ce
risque :

- pendant un flux, il passe la boucle ESPHome en haute fréquence
  (`HighFrequencyLoopRequester`) et vide le socket à chaque appel (64 paquets
  max) ;
- il demande un buffer de réception de 32 Ko (`SO_RCVBUF`) ;
- il écarte les paquets des autres flux (micros des autres devices sur le même
  port) dès la lecture du nom.

Il faut aussi agrandir la file dans la config ESP-IDF :

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_LWIP_UDP_RECVMBOX_SIZE: "32"   # 6 par défaut
      CONFIG_LWIP_SO_RCVBUF: "y"            # pour que SO_RCVBUF soit pris en compte
```

Sans `CONFIG_LWIP_SO_RCVBUF`, le receiver loggue un warning au boot
(`setsockopt(SO_RCVBUF) failed`) et continue de fonctionner.

## Devices testés

- M5Stack Atom Echo S3R (ESP32-S3, codec ES8311)

## Compatibilité receivers

- VoiceMeeter (Incoming Streams) — Banana et Potato
- VBAN Receptor (Windows / Android payant)
- VBAN Receptor X
- Wyoming-STT-VBAN addon Home Assistant
