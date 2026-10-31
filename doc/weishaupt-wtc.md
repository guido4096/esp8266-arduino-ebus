# Weishaupt WTC met drie stookkringen

Gebruik `config/weishaupt-wtc.json` met de firmware uit deze werkboom.
Een nieuwe firmwareflash is nodig: deze versie voegt temperatuurprofielen toe
voor gehele graden en tienden van graden en bouwt de Weishaupt 5023-writes op.
Een JSON-upload alleen is dus niet voldoende.

## Opgenomen waarden

- Ketel: actuele vertrek- en rookgastemperatuur (actieve reads, elke 10 seconden).
- Buiten: temperatuur uit de 500A-broadcast (passief).
- Warmwaterboiler: temperatuur uit 500A (payloadpositie 8, DATA1C), door de
  gebruiker bevestigd aan de hand van de werkelijke warmwatertemperatuur.
- Kring 2 beneden en kring 3 boven: ruimtetemperatuur, actuele en gewenste
  vertrektemperatuur uit 5014 (passief, ongeveer elke 20 seconden).
- Kring 2, 3 en 4: normale en verlaagde gewenste ruimtetemperatuur (actieve reads
  elke 60 seconden, schrijfbaar van 5 tot 30 graden).
- Kring 4 vloerverwarming: waarschijnlijke actuele vertrektemperatuur op adres
  53 (actieve read elke 30 seconden, vergelijken met het scherm).

De actieve gewenste ruimtetemperatuur en de gewenste vertrektemperatuur van
kring 4 zijn nog niet geïdentificeerd en ontbreken. Normaal/verlaagd wijzigen
verandert de opgeslagen instellingen; het schakelt niet de bedrijfsmodus of
het tijdprogramma om. Er wordt niet automatisch aangenomen welke instelling
momenteel actief is.

## Flashen en commandoset vervangen

Vanuit de projectmap, met het juiste USB-poortpad:

```sh
/Users/guido/.platformio/penv/bin/pio run -e esp32-c3-internal --target upload --upload-port /dev/cu.usbmodem14201
```

Na herstart, vervang indien nodig het IP-adres:

```sh
EBUS_URL="http://192.168.10.195"
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{}' "$EBUS_URL/api/v1/app/commands/remove" &&
curl -f -X POST -H 'Content-Type: application/json' \
  --data-binary @config/weishaupt-wtc.json \
  "$EBUS_URL/api/v1/app/commands/upload"
```

Dit verwijdert de bestaande commandoset en slaat vervolgens de nieuwe op.
Upload zonder verwijderen voegt commando's toe of vervangt dezelfde keys.

## Ruimtetemperatuur instellen

Waarden zijn graden Celsius; vermenigvuldig ze niet zelf met tien.
Onderstaande vier voorbeelden zetten beneden op 21/16 en boven op 17/13:

```sh
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc2_normal","value":21}' \
  "$EBUS_URL/api/v1/app/values/write"
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc2_reduced","value":16}' \
  "$EBUS_URL/api/v1/app/values/write"
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc3_normal","value":17}' \
  "$EBUS_URL/api/v1/app/values/write"
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc3_reduced","value":13}' \
  "$EBUS_URL/api/v1/app/values/write"
```

Een succesvol API-antwoord betekent dat de write is ingepland. Controleer de
teruggelezen waarde en het WCM-FS-scherm om te zien dat de instelling is
geaccepteerd. Na een HTTP- of MQTT-write wordt een afzonderlijk leesverzoek ingepland.
De teruglezing is asynchroon; de gewone periodieke uitlezingen blijven actief.

```sh
curl -f "$EBUS_URL/api/v1/app/values"
```

## Schrijfopbouw en verificatie

De vier schrijfcommando's hebben een prefix van zeven bytes:
`destination 50 23 09 00 parameter_lo parameter_hi`.
`Command::buildWriteSequence` voegt de twee waardebytes en `5d010000` toe en
vervangt de nul op payloadpositie 1 door de berekende interne checksum.
Alleen commando's met `"write_type": "weishaupt"` gebruiken deze opbouw.
Deze opbouw geldt voor HTTP, MQTT en cron. Andere schrijfcommando's behouden
hun bestaande opbouw: prefix gevolgd door de gecodeerde waarde.

Het checksumalgoritme gebruikt polynomial 0x5c met ingeschoven databits,
initialisatie nul, over parameter, waarde en suffix. Het is vergeleken met
alle 16 5023-telegrammen uit de log van 17:25:49; alle kwamen overeen.
De vier temperatuurwrites worden ook door hosttests met de volledige
vastgelegde telegrammen vergeleken. Dit is nog geen test van writes door de
nieuwe firmware op de fysieke installatie.

## Kring 4: bevestiging uit log 17:39:12

De instellingen van kring 4 worden gelezen op slave 1c en geschreven naar
master 17. Dit is een ander adres dan mengmodule 53.

- `ff17502309c40500fa005d010000`: normaal naar 25 graden.
- `ff17502309cc0800be005d010000`: verlaagd naar 19 graden.
- `ff1c502203700500 / 02fa00`: normaal teruggelezen als 25 graden.
- `ff1c502203580800 / 02be00`: verlaagd teruggelezen als 19 graden.

Deze twee commando's gebruiken dezelfde firmwareondersteuning als kring 2/3;
na die firmwareflash volstaat een nieuwe JSON-upload. Bij weersafhankelijke
regeling zijn dit referentieruimtetemperaturen, geen gemeten ruimtetemperatuur.

```sh
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc4_normal","value":25}' \
  "$EBUS_URL/api/v1/app/values/write"
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc4_reduced","value":19}' \
  "$EBUS_URL/api/v1/app/values/write"
```

## Handmatig uitlezen

De read-API plant een fysiek leesverzoek in; hij retourneert niet meteen de
nieuwe meetwaarde. Lees na ontvangst van het bustelegram de waardenlijst op:

```sh
curl -f -X POST -H 'Content-Type: application/json' \
  --data '{"key":"hc2_normal"}' "$EBUS_URL/api/v1/app/values/read"
curl -f "$EBUS_URL/api/v1/app/values"
```

Dit vereist de firmwarefix voor handmatige reads: alleen `setLast(0)` wijzigt
de eigen pollplanning van de eBUS-library niet. De fix verstuurt een one-shot
request en koppelt het antwoord via het sessienummer aan het commando.
Passieve commando's weigeren een handmatige read met HTTP 400: hun read_cmd
kan een onvolledig zoekpatroon zijn dat niet als telegram verstuurd mag worden.
