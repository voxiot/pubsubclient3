# Cliente MQTT para Arduino

[English](README.md) | **Português (Brasil)**

Esta biblioteca fornece um cliente para troca de mensagens por publicação e assinatura com um servidor que suporte MQTT.

## Notas deste fork

Esta biblioteca deriva de [knolleary/pubsubclient v2.8](https://github.com/knolleary/pubsubclient/releases/tag/v2.8) e do fork mantido [hmueller01/pubsubclient3](https://github.com/hmueller01/pubsubclient3).

A partir da **v4.0.0**, este repositório (`voxiot/pubsubclient3`) **segue uma linha independente** do projeto de origem. Ele é mantido para a linha de produtos Voxiot e prioriza operação robusta, contínua e compatível com o watchdog em conexões instáveis de WiFi, Internet e broker, em dispositivos com recursos limitados (ESP8266 / ESP32). O caminho de recepção foi reformulado para permitir que o MQTT não paralise o laço principal — importante quando o dispositivo precisa continuar executando funções locais/offline enquanto o broker está lento ou inacessível. A recepção incremental precisa ser habilitada explicitamente, conforme descrito abaixo.

> [!IMPORTANT]
> **Fork independente.** Baseado em `hmueller01/pubsubclient3 v3.3.2`, com versionamento próprio a partir da **v4.0.0**.
>
> Não acompanhamos mais cada versão do projeto de origem. As alterações são avaliadas individualmente e incorporadas apenas quando fazem sentido para este fork. As assinaturas dos métodos existentes são preservadas; copiar um PubSubClient que possui recursos agora é explicitamente proibido (veja as correções de segurança operacional ainda não lançadas abaixo). Os recursos opcionais de resiliência da v4 precisam ser habilitados explicitamente. O comportamento padrão para pacotes grandes demais e as correções de escrita de saída são aplicados automaticamente.

## Resiliência e operação não bloqueante (v4)

Os recursos opcionais abaixo preservam os padrões clássicos. Por padrão, um pacote recebido que exceda o buffer encerra a conexão (veja `setOversizedPolicy`). As correções de escrita de saída são aplicadas automaticamente, incluindo o tratamento de falhas no nível do pacote da v4.0.2.

- **Recepção não bloqueante, habilitada explicitamente.** Por padrão, a recepção é síncrona: uma única chamada a `loop()` pode bloquear durante a leitura de um pacote. Para firmwares que precisam continuar respondendo, por exemplo, a um botão que abre um portão usando dados locais enquanto o broker está indisponível, habilite a recepção incremental com um buffer dedicado:

  ```c
  client.setRxBufferSize(512);     // buffer RX dedicado; habilita recepção não bloqueante
  client.setMaxBytesPerLoop(256);  // opcional: limita os bytes processados por loop()
  ```

  Assim, `loop()` consome apenas os bytes já disponíveis e retorna, remontando os pacotes ao longo das chamadas. O buffer RX é separado do buffer de envio; uma publicação ou keepalive de saída não corrompe uma recepção em andamento. Aloque-o uma vez, no início, antes do TLS, para evitar fragmentação do heap. Se a alocação falhar, o cliente continua usando a recepção bloqueante clássica. Também funciona com `setStream()`, encaminhando o payload incrementalmente.

- **Prazo de leitura do pacote separado do timeout do socket.** `setSocketTimeout()` limita a espera por um único byte; `setPacketTimeout()` limita o tempo de chegada do pacote inteiro. Um timeout curto do socket reduz o tempo de bloqueio do laço, enquanto um timeout separado do pacote permite receber mensagens maiores em conexões lentas. Um pacote parcialmente recebido cuja recepção fique paralisada é abortado; na recepção incremental, não há espera bloqueante pelos bytes restantes.

  ```c
  client.setSocketTimeout(2);   // espera curta por byte; melhora a resposta do loop
  client.setPacketTimeout(10);  // 0 (padrão) = usa o timeout do socket
  ```

- **Política para pacotes grandes demais.** Quando um pacote recebido não cabe no buffer:

  ```c
  client.setOversizedPolicy(MQTT_OVERSIZED_DISCONNECT); // padrão v4: encerra; reconectar ressincroniza
  client.setOversizedPolicy(MQTT_OVERSIZED_IGNORE);     // clássico: descarta e mantém a conexão
  ```

  Em conexões instáveis, um campo Remaining Length excessivo pode indicar corrupção ou dessincronização, além de uma mensagem realmente grande. Quando o fluxo está dessincronizado, é necessário reconectar para ressincronizá-lo; por isso, encerrar a conexão é o padrão. Um pacote maior que `MQTT_MAX_SANE_PACKET_SIZE` é sempre considerado inválido e encerra a conexão, independentemente da política. O limite varia por plataforma: 16 KiB no ESP8266, 64 KiB no ESP32 e o máximo do protocolo nas demais; pode ser substituído com `-D`.

- **Metadados da mensagem recebida.** Dentro do callback, `getRxQos()`, `getRxRetained()` e `getRxDup()` expõem o QoS e os indicadores retain e DUP da mensagem atual.

Veja [CHANGELOG.md](CHANGELOG.md) para a lista completa de alterações e correções da v4.

### Falhas no envio de PUBLISH: antes e depois da v4.0.2

Um PUBLISH pode exigir várias escritas de rede: `beginPublish()` envia o cabeçalho MQTT, o tópico e o identificador opcional do pacote; `write()` / `write_P()` acumulam o payload no buffer; o esvaziamento do buffer e `endPublish()` enviam os bytes restantes. O cabeçalho informa o tamanho do pacote completo antes do envio do payload.

**Antes da v4.0.2**, a decisão de manter ou encerrar a conexão considerava apenas a chamada atual de escrita na rede. Se o cabeçalho já tivesse sido enviado, mas uma escrita posterior do payload não aceitasse nenhum byte até o timeout, a biblioteca retornava falha e descartava o payload acumulado, mantendo a conexão aberta. O broker ainda aguardava esse payload e poderia interpretar o próximo pacote MQTT como sua continuação, corrompendo os limites dos pacotes e eventualmente relatando um pacote malformado. A mesma falha existia entre blocos de transferência completos quando `MQTT_MAX_TRANSFER_SIZE` estava habilitado.

**A partir da v4.0.2**, o caminho de escrita também considera se uma parte anterior do mesmo pacote MQTT já foi enviada:

| Resultado da escrita | Antes da v4.0.2 | v4.0.2 |
| --- | --- | --- |
| Nenhum byte do pacote inteiro é enviado; o transporte permanece conectado | Retorna falha e mantém a conexão | Sem alteração: retorna falha e mantém a conexão |
| O cabeçalho é enviado; a escrita do payload envia zero bytes e não consegue concluir | Retorna falha, mas deixa um pacote incompleto em uma conexão aberta | Retorna falha e encerra a conexão com `MQTT_CONNECTION_LOST` |
| Um bloco completo é enviado; o próximo bloco envia zero bytes e não consegue concluir | Poderia manter a conexão apesar do pacote incompleto | Encerra a conexão com `MQTT_CONNECTION_LOST` |
| Uma escrita envia alguns bytes e depois não consegue concluir | Encerra a conexão | Sem alteração: encerra a conexão |
| Escritas parciais conseguem concluir dentro do timeout existente | Conclui a escrita e mantém a conexão | Sem alteração |

Por exemplo, `publish("topic", "payload")` com QoS 0 envia um prefixo de nove bytes contendo cabeçalho/tópico, seguido de sete bytes de payload. Se os nove bytes do prefixo forem aceitos e nenhum byte do payload puder ser enviado, a v4.0.2 encerra a conexão. Outra publicação não poderá ser anexada ao pacote incompleto; a aplicação deverá reconectar antes de publicar novamente. Os dados pendentes do payload são descartados, em vez de reutilizados após a reconexão. Isso não adiciona retransmissão automática de mensagens nem comprova a entrega ao broker.

Se a primeira escrita no transporte aceitar zero bytes e nenhuma parte do pacote tiver sido enviada, a operação retorna falha imediatamente, sem repetir tentativas até o timeout do socket. A aplicação pode tentar novamente em uma próxima iteração do laço. Isso evita um laço adicional de tentativas na biblioteca; a chamada ao `Client::write()` subjacente ainda pode bloquear conforme o comportamento do transporte. Escritas que continuam um pacote já iniciado mantêm o timeout de tentativas existente e encerram a conexão se o envio ficar incompleto.

A correção abrange `publish()`, `publish_P()`, o fluxo `beginPublish()` / `write()` / `endPublish()`, QoS 0/1/2, payloads maiores que o buffer e escritas divididas em blocos. Preserva a API pública e o prazo de tentativas definido por `setSocketTimeout()`, sem adicionar campos por instância, buffers ou alocações dinâmicas.

`setOversizedPolicy()` controla **somente pacotes recebidos grandes demais**. Não controla falhas de transmissão de saída: depois que o envio de um pacote começa, abandonar os bytes restantes exige encerrar a conexão para restabelecer os limites dos pacotes em uma nova conexão.

A cobertura de regressão está em `tests/src/publish_spec.cpp`, incluindo falhas de payload com zero bytes após um cabeçalho completo, falhas entre buffers de payload e blocos de transferência, bloqueio de publicações posteriores e reconexão sem resíduos do pacote anterior. O caso específico de blocos é habilitado quando o executável de teste e a biblioteca são compilados com `MQTT_MAX_TRANSFER_SIZE` definido, por exemplo, como `4`.

### Integridade de sessões e pacotes (ainda não lançado)

Estas correções se aplicam à recepção bloqueante e à incremental. Não alteram a política de rede/OFFLINE da aplicação nem o timeout existente do socket.

| Caso | Antes | Agora |
| --- | --- | --- |
| O callback desconecta e libera/redimensiona buffers | Um ACK posterior poderia acessar memória liberada | Alterações de buffer e reconexão são recusadas durante o callback; o ACK usa bytes locais e não é enviado após a desconexão |
| Cópia de um cliente | Copiava ponteiros proprietários compartilhados; liberação duplicada na destruição | Construtor de cópia e atribuição são excluídos; passe clientes por referência/ponteiro |
| A recepção bloqueante para no meio do pacote | Retornava com socket aberto e dessincronizado | Encerra com `MQTT_CONNECTION_TIMEOUT`; a próxima conexão começa no limite de um pacote |
| Reconexão após recepção fragmentada | O estado antigo de cabeçalho/payload/descarte poderia consumir dados da nova sessão | Todo o estado do parser é reiniciado ao fechar e antes de conectar |
| Pacote QoS 1/2 malformado sem identificador | O indicador de callback ocupado poderia permanecer ativo indefinidamente | Valida o identificador completo e diferente de zero antes de entrar no callback; a proteção é desativada ao sair |
| PUBLISH manual sobreposto a `loop()` ou a outra operação | ACK, ping ou outro cabeçalho poderiam virar bytes do payload | Controla o pacote ativo e o payload restante; adia RX/keepalive até `endPublish()` |
| Overflow no cálculo do tamanho do PUBLISH | O tamanho poderia sofrer overflow e gerar um pacote malformado | Valida por subtração antes de somar; rejeita tamanhos fora dos limites do protocolo/buffer |
| Limite de IGNORE menor que o buffer da recepção bloqueante | O pacote ainda poderia chegar ao callback | Consome e descarta o pacote usando o limite configurado, como na recepção incremental |
| PUBLISH QoS 2 recebido em duplicidade | O callback poderia executar novamente antes de PUBREL | Controla identificadores pendentes, repete PUBREC sem repetir o callback e libera o identificador em PUBREL |
| Tópicos vazios ou maiores que 65535 bytes | Poderia enviar SUBSCRIBE/UNSUBSCRIBE/PUBLISH malformado | Rejeita sem enviar; strings de CONNECT e escritas internas de strings também têm limites verificados |

`beginPublish()` / `write()` / `endPublish()` constitui uma única transação síncrona de pacote. Conclua-a prontamente: enquanto estiver ativa, `loop()` apenas verifica a conexão e adia o processamento do protocolo. Publicações aninhadas, subscribe/unsubscribe, reconexão e alterações de buffer são recusados. `write()` fora dessa transação retorna zero. Uma escrita maior que o payload restante é rejeitada integralmente, retornando zero; escritas menores ainda podem completá-lo. `endPublish()` com bytes faltando retorna false e encerra com `MQTT_CONNECTION_LOST`. `disconnect()` durante uma transação incompleta fecha o socket diretamente, sem inserir um DISCONNECT no payload. Mesmo um payload de tamanho zero exige `endPublish()`.

Um callback pode publicar de forma síncrona ou desconectar. Deve concluir qualquer publicação manual antes de retornar; caso contrário, a conexão é encerrada para evitar a inserção de um ACK no payload. `setBufferSize()` e `setRxBufferSize()` retornam false enquanto um callback ou uma publicação manual estiver ativo, inclusive depois que o callback chamar `disconnect()`. Alterações de endpoint/transporte/stream são ignoradas durante essas operações. Os ponteiros de tópico/payload continuam sendo temporários: uma publicação dentro do callback pode sobrescrever os dados; copie antes o que precisar preservar. O cliente continua sendo de uso por uma única thread; essas proteções não fornecem sincronização entre threads.

O controle de QoS 2 recebido usa `MQTT_RX_QOS2_MAX_INFLIGHT` posições fixas de `uint16_t` (padrão: **1**, dois bytes por posição), sem armazenar o payload. Defina essa macro consistentemente na biblioteca e na aplicação para suportar mais trocas de confirmação de entrada simultâneas. Se todas as posições estiverem ocupadas, o cliente encerra a conexão antes de entregar ou confirmar um novo identificador; nunca descarta o controle de uma troca ainda incompleta para dar lugar a outra. Isso não adiciona suporte à retransmissão de QoS de saída. A saída para stream contém os dados brutos recebidos e pode incluir payload retransmitido; a supressão de duplicatas se aplica ao callback de mensagens.

Com `cleanSession=false`, a biblioteca também mantém uma cópia do ID do cliente (`strlen(id)+1` bytes, alocados ao preparar a conexão, não a cada mensagem) para distinguir sessões persistentes. Os identificadores pendentes sobrevivem à perda do transporte apenas para o mesmo ID do cliente e endpoint/transporte configurados. São preservados após a reconexão somente se o CONNACK do MQTT 3.1.1 confirmar Session Present. Uma sessão limpa/nova ou a alteração do ID do cliente, broker ou transporte limpa esses identificadores. Reaplicar o mesmo endpoint/transporte os preserva. MQTT 3.1 não permite confirmar a continuidade da sessão; portanto, o controle é reiniciado após uma reconexão bem-sucedida. O estado fica na RAM e é perdido ao reiniciar/destruir o cliente; isso não fornece entrega durável exatamente uma vez à aplicação. As sessões limpas padrão não alocam uma cópia do ID do cliente.

O retorno imediato quando a primeira escrita aceita zero bytes e o prazo de tentativas de um pacote já iniciado permanecem inalterados. `setOversizedPolicy(MQTT_OVERSIZED_IGNORE)` continua preservando uma conexão cujo pacote tenha sido consumido e descartado corretamente; pacotes incompletos e falhas de envio que deixem um pacote parcialmente transmitido exigem encerrá-la para restabelecer os limites dos pacotes.

Cobertura de regressão: `tests/src/resilience_spec.cpp`, executado por `make test`, cobre as dez correções em ambos os modos de recepção quando aplicável, a ordem dos bytes no transporte, reconexão, identidade de sessões persistentes e capacidade de QoS 2. O transporte simulado usa contadores `size_t` para percorrer arrays, evitando overflow no laço de escrita de um tópico válido com 65535 bytes. Execute a suíte também com `MQTT_MAX_TRANSFER_SIZE=4` e com AddressSanitizer para verificar limites entre blocos e o tempo de vida da memória.

## Exemplos

A biblioteca inclui vários sketches de exemplo. No aplicativo Arduino, veja Arquivo > Exemplos > PubSubClient.

A documentação completa da API está disponível em: https://hmueller01.github.io/pubsubclient3/api

## Limitações

- O cliente é baseado na [especificação MQTT versão 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html), com algumas limitações.
- Pode publicar com QoS 0 e, desde a [v3.2.0](https://github.com/hmueller01/pubsubclient3/releases/tag/v3.2.0), também com QoS 1 ou 2.

  **ATENÇÃO:** Não há suporte à retransmissão MQTT de saída, para manter baixo o consumo de memória da biblioteca.

  Sem retransmissão, o QoS da publicação continua influenciando a entrega do broker ao assinante, respeitando o QoS da assinatura, mas não fornece por si só a recuperação completa de uma publicação interrompida entre cliente e broker. O MQTT usa TCP, que retransmite bytes durante uma conexão; isso não substitui a retransmissão MQTT após perda de conexão.
- Pode assinar tópicos com QoS 0 ou QoS 1.
- O tamanho máximo padrão da mensagem, incluindo o cabeçalho, é **256 bytes**. Pode ser configurado por `MQTT_MAX_PACKET_SIZE` em `PubSubClient.h` ou por `PubSubClient::setBufferSize(size)`. Por padrão na v4, um pacote recebido maior que o buffer encerra a conexão; veja `setOversizedPolicy()` para restaurar o comportamento clássico de ignorar e manter a conexão.
- O intervalo padrão de keepalive é 15 segundos. Pode ser configurado por `MQTT_KEEPALIVE` em `PubSubClient.h` ou por `PubSubClient::setKeepAlive(keepAlive)`.
- O cliente usa MQTT 3.1.1 por padrão. Para usar MQTT 3.1, altere `MQTT_VERSION` em `PubSubClient.h`.
- Desde a [v3.3.0](https://github.com/hmueller01/pubsubclient3/releases/tag/v3.3.0), é possível publicar e assinar tópicos em `PROGMEM` ou `__FlashStringHelper`. Veja o exemplo [mqtt_progmem](https://github.com/hmueller01/pubsubclient3/blob/aae84e4d1aa65e752e19e30239b5796b4fe2705b/examples/mqtt_progmem/src/mqtt_progmem.cpp#L39-L48).

  Para publicar tópicos em `PROGMEM`, use:

  ```c
  const char TOPIC[] PROGMEM = "test";
  const char HELLO_WORLD[] PROGMEM = "hello world";
  client.beginPublish_P(TOPIC, strlen_P(HELLO_WORLD), MQTT_QOS0, false);
  client.write_P(HELLO_WORLD);
  client.endPublish();
  ```

  `client.publish_P(...)` já é usado para payloads em `PROGMEM`.

## Hardware compatível

A biblioteca usa a API Arduino Ethernet Client para interagir com o hardware de rede subjacente. Isso permite o uso com diversas placas e shields, incluindo:

- Arduino Ethernet
- Arduino Ethernet Shield
- Arduino YUN — use o `YunClient` incluído no lugar de `EthernetClient` e execute `Bridge.begin()` primeiro.
- Arduino WiFi Shield — para enviar pacotes maiores que 90 bytes com esse shield, habilite `MQTT_MAX_TRANSFER_SIZE` em `PubSubClient.h`.
- Sparkfun WiFly Shield — [biblioteca](https://github.com/dpslwk/WiFly)
- TI CC3000 WiFi — [biblioteca](https://github.com/sparkfun/SFE_CC3000_Library)
- Intel Galileo/Edison
- ESP8266
- ESP32

A biblioteca atualmente não pode ser usada com hardware baseado no chip ENC28J60, como Nanode ou Nuelectronics Ethernet Shield. Para esses dispositivos, existe uma [biblioteca alternativa](https://github.com/njh/NanodeMQTT).

## Histórico de alterações

Veja [CHANGELOG.md](CHANGELOG.md).

## Licença

Este código é distribuído sob a [licença MIT](LICENSE.txt).
