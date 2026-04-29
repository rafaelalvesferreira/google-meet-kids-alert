# Meet Alert

Indicador de agenda baseado em ESP32-C6 e matriz de LED 8x8. Consulta sua
agenda do Google e pinta a matriz:

| Estado | Forma | Significado |
|---|---|---|
| Verde | Círculo cheio | Livre (dentro do horário comercial) |
| Amarelo | Círculo cheio | Em reunião sozinho (sem convidados) |
| Vermelho | Círculo cheio | Em reunião com convidados |
| Amarelo piscando | Círculo cheio piscando | Reunião vermelha começando em ≤ 5 min |
| Roxo | Círculo cheio | Erro / sem configuração |
| Apagado | — | Fora do horário comercial — dispositivo em deep sleep |

A família toda usa o mesmo template circular; o que muda é a cor (e o
amarelo pisca). Um LED de canto mostra a carga da bateria.

```
VERDE / AMARELO       VERMELHO              AMARELO BLINK

. . X X X X . .      . . R R R R . .      . . Y Y Y Y . .
. X X X X X X .      . R R R R R R .      . Y Y Y Y Y Y .
X X X X X X X X      R R R R R R R R      Y Y Y Y Y Y Y Y
X X X X X X X X      R R R R R R R R      Y Y Y Y Y Y Y Y
X X X X X X X X      R R R R R R R R      Y Y Y Y Y Y Y Y
X X X X X X X X      R R R R R R R R      Y Y Y Y Y Y Y Y
. X X X X X X .      . R R R R R R .      . Y Y Y Y Y Y .
. . X X X X . .      . . R R R R . .      . . Y Y Y Y . .
```

---

## Sumário

- [Materiais](#materiais)
- [Arquitetura de energia](#arquitetura-de-energia)
- [Esquema elétrico](#esquema-elétrico)
- [Montagem física](#montagem-física)
- [Google Apps Script](#google-apps-script)
- [Firmware — build e upload](#firmware--build-e-upload)
- [Primeiro uso — configuração](#primeiro-uso--configuração)
- [Uso diário](#uso-diário)
- [Botão BOOT](#botão-boot)
- [Personalização](#personalização)
- [Troubleshooting](#troubleshooting)

---

## Materiais

| Qtd | Item |
|---:|---|
| 1 | Seeed Studio XIAO ESP32-C6 |
| 1 | Matriz LED WS2812 5050 RGB 8x8 (64 LEDs) |
| 1 | Módulo boost DC-DC 3.7V → 5V (aquele mini com saída selecionável) |
| 1 | Bateria LiPo 3.7V (qualquer capacidade, 500-2000mAh é confortável) |
| 2 | Resistor 100kΩ (1/4W, tolerância 1% de preferência) |
| 1 | Resistor 330Ω ou 470Ω (linha de dados do WS2812) |
| 1 | Capacitor eletrolítico 1000µF / 10V ou 16V (alimentação da matriz) |

Fios finos, solda e opcional: caixa/suporte impresso em 3D.

---

## Arquitetura de energia

O pino `5V` do XIAO **só fica vivo quando o USB está plugado**. Em modo
bateria ele fica sem tensão. Por isso a matriz é alimentada pelo **boost
converter**, não pelo pino `5V`.

A bateria faz um papel duplo: ela alimenta o boost converter (que alimenta a
matriz) e também o próprio XIAO. Quando o USB está plugado, o XIAO carrega
a bateria automaticamente (ele tem CI de carga embutido) — a matriz continua
puxando do boost, que continua puxando da bateria. Transição contínua.

```
      BAT+ ┬──► Boost converter (saída = 5V) ──► WS2812 VCC
           │
           └──► Divisor 100k / 100k ────────────► ADC (D0)

      USB ────► XIAO (carrega bateria automaticamente)
```

> **Regra:** mantenha a bateria sempre conectada. Sem bateria, a matriz não
> recebe alimentação mesmo com USB plugado.

---

## Esquema elétrico

```
                          ┌─────────────────────────────┐
                          │      XIAO ESP32-C6          │
                          │                             │
    LiPo (+) ─────┬─────► │ BAT+                        │
                  │       │                             │
                  │       │ BAT- ◄─── LiPo (-)          │
                  │       │                             │
                  │       │ D0 (ADC)  ◄── divisor bat   │
                  │       │                             │
                  │       │ D6 (GPIO16) ── via 330Ω ──► WS2812 DIN
                  │       │                             │
                  │       │ GND ◄──── GND comum         │
                  │       │                             │
                  │       └─────────────────────────────┘
                  │
                  ▼
           ┌──────────────┐
           │ Boost 3.7→5V │ ── saída 5V ─┬──► WS2812 VCC
           │              │              │
           └──────┬───────┘             1000µF (entre VCC e GND)
                  │                      │
                  ▼                      ▼
                 GND ──── GND comum ─── GND

  Divisor de bateria (monitoramento):

  LiPo(+) ──┬─[ R1 = 100kΩ ]──┬──► D0 (ADC)
            │                 │
            │                 └──[ R2 = 100kΩ ]─── GND
            │                                       │
            │                               [ 1MΩ ]─┘  (opcional, anti-float)
```

### Valores explicados

- **Divisor 100k + 100k:** corta a tensão pela metade (LiPo pode chegar a
  4.2V; o ADC aceita até ~3.3V). Corrente vazada de ~21µA, desprezível.
- **Resistor 330-470Ω na linha de dados:** absorve overshoot/refletion no
  pulso WS2812, melhora confiabilidade. Fica entre o pino `D6` e o `DIN` da
  matriz, o mais perto possível da matriz.
- **Capacitor 1000µF:** estabiliza o 5V quando vários LEDs trocam de cor
  simultaneamente. Ligue os terminais no VCC e GND da matriz.
- **Resistor 1MΩ (opcional):** se a bateria for desconectada, o pino ADC
  fica flutuando. Esse resistor puxa para GND, mantendo a leitura previsível.
  Se não colocar, o firmware tenta detectar "sem bateria" via tensão baixa.

---

## Montagem física

1. **Configure o boost converter para 5V.** Tem um jumper ou chave
   seletora; coloque em `5V`. Antes de ligar qualquer LED, teste com
   multímetro.
2. **Solde os fios:**
   - LiPo `+` → entrada `+` do boost **e** `BAT+` do XIAO **e** lado quente
     do divisor (R1).
   - LiPo `-` → entrada `-` do boost **e** `BAT-` do XIAO.
   - Saída `+` do boost → `VCC` da matriz.
   - Saída `-` do boost → `GND` da matriz **e** `GND` do XIAO (todos os GNDs
     em comum).
   - Divisor: R1 e R2 em série entre LiPo `+` e GND, com o ponto do meio no
     pino `D0` do XIAO.
   - `D6` do XIAO → resistor 330Ω → `DIN` da matriz.
   - Capacitor 1000µF entre `VCC` e `GND` da matriz, **respeitando
     polaridade** (o lado marcado com faixa/negativo vai no GND).
3. **Primeiro teste:** sem a matriz conectada, ligue a bateria. O LED
   vermelho do XIAO deve acender (carregando, se USB estiver plugado) ou
   deve estar apagado (bateria cheia ou sem USB). Se o LED estiver pisca em
   padrão estranho, é porque a bateria está em curto — desligue.
4. **Teste da matriz:** plugue o USB, suba o firmware (próxima seção) e
   observe.

---

## Google Apps Script

O firmware busca uma URL pública que retorna `green`, `yellow`, `red` ou
`yellow_blink`. Essa URL é um Apps Script que lê sua agenda.

### Passos

1. Abra <https://script.google.com> e clique em **Novo projeto**.
2. Apague o conteúdo padrão e cole:

```javascript
const TOKEN = 'troque-por-um-segredo-qualquer';
const WARNING_MIN = 5;

function doGet(e) {
  if (e.parameter.token !== TOKEN) return out('unauthorized');

  // Alerta de bateria fraca — cria tarefa no Google Tasks
  if (e.parameter.action === 'battery') {
    const listas = Tasks.Tasklists.list().items;
    if (listas && listas.length > 0) {
      const task = {
        title: 'Carregar bateria do Meet Alert',
        notes: 'Bateria abaixo de 10%. Conecte ao carregador.',
        due: new Date().toISOString()
      };
      Tasks.Tasks.insert(task, listas[0].id);
    }
    return out('task_created');
  }

  const now = new Date();
  const janela = new Date(now.getTime() + 30 * 60 * 1000);

  const eventos = CalendarApp.getDefaultCalendar()
    .getEvents(now, janela)
    .filter(ev => !ev.isAllDayEvent())
    .filter(ev => ev.getMyStatus() !== CalendarApp.GuestStatus.NO);

  const agora = eventos.filter(ev =>
    ev.getStartTime() <= now && ev.getEndTime() > now
  );

  // Focus Time ativo força estado vermelho
  if (agora.some(ev => ev.getEventType() === CalendarApp.EventType.FOCUS_TIME)) return out('red');

  // 1. Reunião vermelha rolando agora
  if (agora.some(ev => ev.getGuestList().length > 0)) return out('red');

  // 2. Vermelha começando em <= 5 min sobrepõe amarelo atual
  const proxVermelha = eventos.find(ev =>
    ev.getStartTime() > now && ev.getGuestList().length > 0
  );
  if (proxVermelha) {
    const minutos = (proxVermelha.getStartTime() - now) / 60000;
    if (minutos <= WARNING_MIN) return out('yellow_blink');
  }

  // 3. Reunião solo rolando agora
  if (agora.length > 0) return out('yellow');

  // 4. Livre
  return out('green');
}

function out(s) {
  return ContentService.createTextOutput(s)
    .setMimeType(ContentService.MimeType.TEXT);
}
```

3. Troque `TOKEN` por um segredo seu (qualquer string — 16+ caracteres,
   alfanuméricos, sem espaços).
4. **Habilite a Tasks API:** no painel esquerdo clique em **Serviços** (`+`),
   procure **Tasks API** e adicione. Sem isso o alerta de bateria não funciona.
5. **Deploy:** botão `Implantar` → `Nova implantação` → engrenagem →
   `Aplicativo da Web`.
   - **Executar como:** `Eu` (sua conta do Google)
   - **Quem pode acessar:** `Qualquer pessoa`
6. Clique em `Implantar`. Aceite as permissões que o Google pedir (Calendar,
   Tasks e execução como Web App). Copie a **URL do aplicativo da Web**, que
   termina em `/exec`.
7. **Teste:** no navegador, abra
   `https://script.google.com/.../exec?token=SEU_TOKEN`. Deve responder
   `green`, `yellow` ou `red`. Sem o token, responde `unauthorized`.

### Quanto isso consome de quota?

- Apps Script free: 20.000 chamadas `URL Fetch` por dia.
- Com polling de 30s dentro do horário: 2.880 chamadas/dia. Folga enorme.
- Fora do horário o dispositivo hiberna e acorda a cada 5 min (configurável) — cai para ~288 chamadas extras por 24h fora do expediente.

---

## Firmware — build e upload

### Pré-requisitos

- [VS Code](https://code.visualstudio.com/) + extensão
  [PlatformIO IDE](https://platformio.org/install/ide?install=vscode).
- Cabo USB-C de **dados** (não só de carga).

### Compilar e subir

1. Abra a pasta do projeto no VS Code.
2. Plugue o XIAO no USB. O PlatformIO deve detectar a porta sozinho (ex:
   `/dev/ttyACM0`, `COM5`).
3. Na barra inferior do VS Code (azul), clique na seta `→` (Upload). Ou
   execute:
   ```bash
   pio run -t upload
   ```
4. Depois, abra o monitor serial pra ver os logs (115200 bauds):
   ```bash
   pio device monitor
   ```

Se der erro dizendo que `seeed_xiao_esp32c6` não existe, edite
`platformio.ini` e troque a linha `platform = ...` por:

```ini
platform = https://github.com/pioarduino/platform-espressif32#stable
```

É um fork com suporte melhor para os ESP32 mais novos.

---

## Primeiro uso — configuração

Ao ligar pela primeira vez, o firmware não tem credenciais salvas. Ele vai:

1. Pintar a matriz de **branco suave** enquanto tenta conectar.
2. Como não há nada salvo, abre um **Access Point** chamado
   `MeetAlert-Config`.
3. A matriz fica **azul sólida**.

Do seu celular:

1. Entre no WiFi `MeetAlert-Config` (sem senha).
2. Um **portal cativo** abre automaticamente (igual WiFi de hotel). Se não
   abrir, navegue para <http://192.168.4.1>.
3. Clique em `Configure WiFi` ou similar.
4. Preencha:
   - **WiFi SSID e senha** da sua rede doméstica/escritório.
   - **URL Apps Script:** a URL `/exec` do passo anterior (sem `?token=...`).
   - **Token:** o mesmo `TOKEN` que você definiu no Apps Script.
   - **Brilho (1-255):** comece com 30. Mais baixo = bateria dura mais.
   - **Polling (seg):** comece com 30.
   - **Sleep fora horário (seg):** intervalo de deep sleep fora do expediente. Padrão 300 (5 min).
   - **Fuso horário:** offset UTC inteiro (ex: `-3` para Brasília, `0` para UTC).
   - **Expediente início / fim (h):** hora de início e fim em número inteiro (ex: `9` e `18`).
   - **Dias úteis (DSTQQSS):** string de 7 dígitos, um por dia da semana a partir de domingo. `1` = dia ativo, `0` = dia off. Padrão `0111110` (seg–sex).
5. Salvar. O device reinicia e conecta.

---

## Uso diário

Depois de configurado:

- Liga → conecta WiFi em 2-3s → começa a pintar estado.
- Consulta o Apps Script no intervalo definido (30s por padrão).
- O piscar do amarelo (alerta de 5min) é feito **localmente** no ESP32 para
  ficar suave independente de latência HTTP.
- LED de canto mostra bateria:
  - Verde: > 50%
  - Laranja: 20-50%
  - Vermelho: 10-20%
  - Vermelho piscando rápido: < 10% (carregue!)
  - Apagado: bateria não detectada / sem bateria
- Quando a bateria cai abaixo de 10%, o firmware cria automaticamente uma
  tarefa **"Carregar bateria do Meet Alert"** no Google Tasks. A tarefa é
  criada uma única vez por ciclo crítico — só volta a criar se a bateria
  recuperar para ≥ 15% e cair novamente. O alerta é enviado durante o poll
  normal (não interrompe as animações da matriz).

---

## Botão BOOT

O firmware usa o botão BOOT do XIAO (ao lado do USB). **Segurando durante a
operação normal:**

| Tempo segurado | Feedback visual | Ação ao soltar |
|---:|---|---|
| < 3 s | Nada | Ignorado |
| 3 s – 5 s | Matriz fica **laranja** | Apaga WiFi salvo → abre portal de config limpo |
| ≥ 5 s | Matriz fica **azul** | Abre portal mantendo WiFi atual (útil pra trocar só o token/URL) |

Assim:

- **Mudou de WiFi?** Segure 3 segundos, solte no laranja → reconfigura tudo.
- **Só quer trocar token ou brilho?** Segure 5+ segundos, solte no azul →
  vai direto pro portal sem perder o WiFi.

Você tem feedback visual claro enquanto segura, então sabe exatamente o que
vai acontecer quando soltar.

---

## Personalização

### Mudar cores ou thresholds de bateria

Abra `src/main.cpp` e ajuste em `batteryColor()`.

### Mudar o threshold do alerta de bateria

O alerta de tarefa dispara quando a bateria fica abaixo de 10% e reseta
quando volta acima de 15%. Para alterar, edite os valores no bloco de poll
dentro de `loop()` em `src/main.cpp`:

```cpp
if (batPct >= 0 && batPct < 10 && !batteryTaskSent) {  // ← limiar de disparo
  ...
} else if (batPct < 0 || batPct >= 15) {               // ← limiar de reset
  batteryTaskSent = false;
}
```

### Mudar o LED que indica bateria

Troque `BATTERY_INDICATOR_LED` (é o índice 0 por padrão — provavelmente o
canto superior esquerdo da sua matriz; pode variar conforme o cabeamento).

### Alterar os desenhos

Os bitmaps 8x8 ficam no topo da seção `MATRIZ` em `src/main.cpp`:
`PATTERN_CIRCLE`. Cada byte é
uma linha, cada bit uma coluna (bit 7 = coluna 0 à esquerda). É literal:
cada `1` = LED ligado com a cor daquele estado, cada `0` = apagado.

### Mudar o horário comercial

Configure diretamente no portal WiFi do dispositivo (segure o botão por 3s+):

- **Fuso horário**, **Expediente início/fim** e **Dias úteis** são os campos relevantes.
- A decisão de dormir é feita no firmware: se o estado for verde e o horário atual estiver fora do expediente configurado, o device entra em deep sleep automaticamente.
- O intervalo de sono é o campo **"Sleep fora horário (seg)"** (padrão 300s = 5 min).

### Janela de aviso diferente de 5 min

Edite `WARNING_MIN` no Apps Script. Não precisa re-flashar o firmware.

### Mais de uma agenda / calendário compartilhado

No Apps Script, troque `CalendarApp.getDefaultCalendar()` por
`CalendarApp.getCalendarById('id-do-calendario@group.calendar.google.com')`.

---

## Troubleshooting

### A matriz não acende

- Confira se o boost está entregando 5V na saída (multímetro).
- Confira se o GND do boost está ligado ao GND do XIAO.
- Confira polaridade do DIN — WS2812 só aceita sinal em um dos lados da
  matriz (tem uma seta na PCB, DIN é onde a seta aponta *de*).
- Teste sem o resistor 330Ω (alguns WS2812 ficam sensíveis a ele em certas
  condições).

### Primeira linha de LEDs estranha, resto ok

Geralmente é problema de sinal no `DIN`. Aproxime o resistor 330Ω da matriz
ou diminua o valor (150-220Ω).

### Cores trocadas (vermelho aparece como verde)

Edite em `main.cpp` a linha `FastLED.addLeds<WS2812, MATRIX_DATA_PIN, GRB>`.
Troque `GRB` para `RGB` ou `BRG` até as cores baterem.

### Desenhos aparecem embaralhados (X vira um traço, círculo distorcido)

Algumas matrizes 8x8 são cabeadas em **zigzag (serpentina)** em vez de
linha-por-linha. O código atual assume ordem progressiva (LED 0 no canto
superior esquerdo, LED 7 no canto superior direito, LED 8 começa a segunda
linha pela esquerda).

Se a sua for serpentina, edite `drawPattern()` em `main.cpp`:

```cpp
void drawPattern(const uint8_t* pattern, CRGB fg) {
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      uint8_t col = (y & 1) ? (7 - x) : x;  // linha ímpar invertida
      uint8_t idx = y * 8 + col;
      bool on = pattern[y] & (1 << (7 - x));
      leds[idx] = on ? fg : CRGB::Black;
    }
  }
}
```

Se a matriz estiver rotacionada/espelhada, dá pra espelhar o bitmap
manualmente ou ajustar o mapeamento de `(x, y) → idx`.

### `unauthorized` ao testar a URL

- Token digitado diferente do que está no Apps Script.
- Copiou a URL errada — deve terminar em `/exec`, não `/dev`.
- Deploy não foi feito como "Qualquer pessoa".

### O XIAO não aparece na porta serial

- Use cabo USB-C **com dados**. Muitos cabos que vêm junto de carregadores
  só têm linhas de energia.
- No macOS pode precisar do driver CH343 ou CP210x (o XIAO ESP32-C6 usa USB
  nativo do chip, então geralmente não precisa de driver).
- Pressione o botão `RESET` enquanto segura `BOOT`, solte `RESET`, solte
  `BOOT` — força modo download.

### A bateria descarrega muito rápido

- Reduza o brilho (1-20 já é bem visível em ambiente interno).
- Aumente o intervalo de polling (60s ou 120s).
- Cores sólidas consomem menos que amarelo (amarelo = R+G ligados).

### HTTP 302 / resposta vazia

O Apps Script redireciona para `googleusercontent.com`. O firmware já chama
`setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)`. Se você mexeu nessa
linha, recoloque — sem ela a resposta fica vazia.
