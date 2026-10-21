# Histórico de alterações

[English](CHANGELOG.md) | **Português (Brasil)**

Todas as alterações relevantes deste projeto serão documentadas neste arquivo.

O formato é baseado em [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), e este projeto segue o [Versionamento Semântico](https://semver.org/).

## Ainda não lançado

## [5.0.0] - 2026-10-21

### Alterado

- Adicionado `MQTT_ENABLE_INCREMENTAL_RX` para todo o build (padrão 0). O perfil bloqueante padrão remove campos, parser e APIs de RX incremental, preservando as proteções da recepção bloqueante; as APIs incrementais exigem ativação explícita com valor 1, e todas as unidades de compilação devem usar o mesmo valor e ser recompiladas. Ajustados os testes dos dois perfis e da disponibilidade da API na compilação.

- Escritas automáticas de protocolo passam a selecionar a política de retry pelo modo de recepção: incremental faz uma tentativa no transporte; bloqueante preserva a conclusão de escritas parciais. Isso troca recuperação de congestionamento transitório por menos esperas internas no modo incremental. Chamadas do transporte e publicações em callbacks ainda podem bloquear; documentados os limites de latência restantes. Não são adicionados buffers de TX pendente, alocações de heap ou estado por cliente.
- `setSocketTimeout(0)` passa a restaurar `MQTT_SOCKET_TIMEOUT` (padrão de 15 segundos), em vez de expirar imediatamente. Timeouts positivos explícitos, incluindo dois segundos, preservam seu significado; `setPacketTimeout(0)` continua usando o timeout do socket. Validados o timeout do socket e a capacidade de QoS 2 recebido na compilação com `static_assert` (de 1 a 65535); mantido um slot de QoS 2 por padrão.
- **Bump major a partir da 4.0.3:** as correções de integridade de pacotes e sessões introduzem incompatibilidades intencionais de código-fonte e comportamento; por isso, esta versão não é classificada como um patch compatível com versões anteriores. As assinaturas dos métodos existentes são preservadas, mas a cópia de clientes e o uso permissivo da publicação manual deixam de ser suportados.
- Excluídos o construtor de cópia e a atribuição por cópia de `PubSubClient` para evitar o compartilhamento da propriedade de ponteiros internos e a liberação duplicada de memória. Adapte os consumidores para usar referências/ponteiros ou crie e configure explicitamente clientes independentes.
- Exigida uma transação completa `beginPublish()` / `write()` / `endPublish()`, incluindo `endPublish()` para payload vazio. Escritas fora da transação e escritas maiores que o payload restante são rejeitadas; finalizar um payload incompleto encerra a conexão com `MQTT_CONNECTION_LOST`. Durante uma transação ativa, RX/keepalive são adiados e operações sobrepostas são recusadas. Um callback deve concluir sua publicação manual antes de retornar.
- Alterações de buffer e reconexão são recusadas durante callbacks; alterações de endpoint/transporte/stream são ignoradas durante callbacks ou publicações manuais para preservar a validade dos buffers ativos e os limites dos pacotes. Consumidores que dependem dessas operações devem movê-las para fora da operação ativa.

### Corrigido

- PUBACK, PUBREC, PUBREL, PUBCOMP e PINGRESP passam por `writeControlResponse()`. A recepção incremental encerra com `MQTT_CONNECTION_LOST` após uma única escrita incompleta; a bloqueante repete tentativas de escritas parciais. Respostas obrigatórias encerram com zero bytes nos dois modos. Aplicada a mesma seleção de retry a PINGREQ, preservando o adiamento com zero bytes e ativando `_pingOutstanding` somente após transmissão completa. A conexão é encerrada antes de processar RX quando um ping parcial falha.
- Reiniciado o estado do parser ao fechar e reconectar; encerradas conexões com leituras bloqueantes interrompidas; validados identificadores de pacote, tamanhos de tópicos e cálculos de tamanho; respeitado o limite de entrada com IGNORE; suprimidas entregas duplicadas de QoS 2 recebido ao callback, preservando identificadores pendentes somente para uma sessão persistente confirmada com a mesma identidade.
- Uma escrita parcial de saída passa a ser completada em vez de abandonar o pacote: um `Client` que aceitasse menos bytes que o solicitado (janela de transmissão cheia ou timeout no meio da escrita) deixava um pacote truncado no transporte. Como o cabeçalho fixo já havia anunciado o tamanho restante, o destinatário interpretava o próximo pacote como continuação do anterior, causando uma dessincronização não detectada da qual os pacotes seguintes não conseguiam se recuperar, embora a conexão aparentasse estar saudável. Os bytes restantes passam a ser reenviados, cedendo a CPU entre tentativas, até o socket aceitá-los, fechar ou atingir o prazo de `setSocketTimeout()`. Abrange `writeBuffer()` (CONNECT/SUBSCRIBE/UNSUBSCRIBE e o esvaziamento do buffer em `endPublish()`, tanto no ramo normal quanto no ramo dividido por `MQTT_MAX_TRANSFER_SIZE`), a escrita de cabeçalho/tópico de `beginPublish()` e, no modo de recepção bloqueante, o PINGREQ enviado por `loop()`.
- Exigida uma reconexão para ressincronização quando o prazo expira após bytes já terem sido enviados. Nessa situação, reconectar é a única forma de recuperar os limites dos pacotes; manter a conexão apenas adiava o problema: o próximo PINGREQ era consumido pelo pacote truncado, e o keepalive encerrava a conexão até dois intervalos depois, com as publicações intermediárias reportadas como enviadas. Uma escrita que não enviou **nenhum** byte preserva os limites do fluxo, mantém a conexão e apenas informa a falha, como antes.
- O laço de escrita de `MQTT_MAX_TRANSFER_SIZE` passa a ceder a CPU com `PUBSUB_WAIT_YIELD()`, adequado à plataforma, no lugar de `yield()` simples, que não atendia ao watchdog no ESP32/FreeRTOS, onde é necessário bloquear por um tick para ceder a CPU.

### Testes

- Substituídas as esperas de relógio real dos testes de keepalive por avanço explícito do relógio de host, preservando os cenários e removendo expectativas de ping dependentes do escalonamento.
- Exercitadas escritas completas, parciais recuperáveis, parciais sem progresso e de zero bytes das cinco respostas obrigatórias nos dois modos, verificando exatamente uma chamada ao transporte no incremental. Cobertos seleção de modo do PINGREQ, adiamento com zero bytes, confirmação, reconexão e ausência de RX após falha de ping. Confirmada a preservação dos retries de publicação síncrona com RX incremental. Verificados timeout padrão/explícito/restaurado e fallback do timeout de pacote com relógio limitado aos testes de host; substituído o timeout zero do fixture por um segundo. Ampliados os testes de capacidade de QoS 2 para duplicatas, reutilização de slots e ausência de confirmação ao exceder a capacidade, executando com um e quatro slots. Adicionado `check_config.py` a `make test` para dezoito configurações de compilação aceitas/rejeitadas, usando somente a biblioteca padrão do Python 3.
- Adicionado `resilience_spec` para validade de buffers durante callbacks, propriedade dos recursos, recuperação do parser, publicação manual, limites e sessões QoS 2 nos dois modos de recepção quando aplicável; corrigidas expectativas de pacotes e utilizados contadores `size_t` no transporte simulado.
- `publish_spec`: uma publicação com todas as escritas parciais ainda entrega exatamente os mesmos bytes; uma escrita que não envia nada faz a publicação falhar, mas mantém a conexão; escritas incompletas do cabeçalho e do payload encerram a conexão com `MQTT_CONNECTION_LOST`. `connect_spec`: uma escrita incompleta de CONNECT faz a conexão falhar e fecha o socket.
- `ShimClient`: `setWriteLimit()` simula um socket que deixa de aceitar bytes; `setWriteChunk()` simula um socket que aceita apenas alguns bytes por chamada.

## [4.0.0] - 2026-10-17

Primeira versão do fork `voxiot/pubsubclient3`, independente de `hmueller01/pubsubclient3` e baseado na versão `v3.3.2` do projeto de origem. Foco em operação robusta, contínua e compatível com o watchdog em conexões instáveis. A API pública permanece compatível com versões anteriores; todos os novos recursos de resiliência precisam ser habilitados explicitamente, exceto o padrão para pacotes grandes demais, que passa a encerrar a conexão (veja `setOversizedPolicy()`).

### Adicionado

- `setMaxIncomingPacket()` / `getMaxIncomingPacket()` para limitar o tamanho máximo aceito de um pacote recebido.
- `setPacketTimeout()` / `getPacketTimeout()` para limitar o tempo total de recepção de um pacote independentemente do timeout de espera por byte do socket.
- Recepção não bloqueante habilitada explicitamente: `setRxBufferSize()` / `getRxBufferSize()` alocam um buffer dedicado para que `loop()` remonte os pacotes incrementalmente, sem esperar pelos bytes restantes. Desativada por padrão; o caminho bloqueante clássico é preservado. Também funciona com Stream, encaminhando o payload incrementalmente.
- `setMaxBytesPerLoop()` / `getMaxBytesPerLoop()` para limitar os bytes recebidos processados em cada chamada de `loop()`, mantendo a resposta do laço principal para funções offline.
- `setOversizedPolicy()` / `getOversizedPolicy()` para escolher o tratamento de pacotes recebidos grandes demais: `MQTT_OVERSIZED_DISCONNECT` (novo padrão, encerra para reconectar) ou `MQTT_OVERSIZED_IGNORE` (comportamento clássico, descarta e mantém a conexão).
- `MQTT_MAX_SANE_PACKET_SIZE`, limite de tamanho plausível adequado à plataforma: 16 KiB no ESP8266, 64 KiB no ESP32 e máximo do protocolo nas demais. Um pacote acima dele é tratado como inválido/corrompido e sempre encerra a conexão, independentemente da política.

### Corrigido

- Tratamento seguro e configurável de pacotes recebidos grandes demais (veja `setOversizedPolicy()`): o novo padrão encerra a conexão imediatamente, permitindo que a reconexão ressincronize o fluxo em caso de corrupção/dessincronização. A política opcional de ignorar consome e descarta o pacote, de forma bloqueante limitada pelo prazo do pacote e com cessão periódica da CPU, ou incrementalmente sem bloquear, preservando a conexão. Tamanhos acima do limite plausível sempre encerram a conexão.
- Adicionado prazo total de leitura do pacote em `readByte()` para impedir que um remetente enviando bytes muito lentamente, como no caso slowloris, retenha `loop()` indefinidamente.
- Cessão cooperativa da CPU durante leituras de grandes volumes de dados para continuar atendendo ao watchdog no ESP32/FreeRTOS.
- Exigida reconexão para ressincronização quando falha a escrita de uma resposta de controle de QoS/ping (PUBACK/PUBREC/PUBREL/PUBCOMP/PINGRESP).
- Evitado comportamento indefinido no retorno de `_nextMsgId` ao início da faixa, causado por leitura/modificação na mesma expressão.
- Protegido `appendBuffer()` contra buffer ausente e `setServer(uint8_t*)` contra ponteiro nulo.
- Limitados `setBufferSize()` / `setRxBufferSize()` a `MQTT_MAX_POSSIBLE_PACKET_SIZE` para rejeitar tamanhos inválidos.
- Corrigidos avisos de `-Wsign-conversion` nos cálculos de tamanho do tópico, msgId e bytes ignorados no Stream, usando deslocamentos explicitamente sem sinal.

### Testes

- Adicionado `fragmented_spec`, cobrindo remontagem não bloqueante byte a byte, `setMaxBytesPerLoop()`, descarte incremental de pacote grande demais com ressincronização (política de ignorar), PUBLISH/PUBACK QoS 1 fragmentado, entrega fragmentada do payload ao Stream, desconexão imediata por pacote grande demais (padrão) e desconexão por tamanho inválido acima do limite plausível.

## [3.3.2] - 2026-09-27

### Corrigido

- Evitada regressão de truncamento em `uint16_t payloadOffset` de `handlePacket()` e em `uint16_t length` de `subscribeImpl()`/`unsubscribeImpl()`, por @router0mail em [#104](https://github.com/hmueller01/pubsubclient3/pull/104).

## [3.3.1] - 2026-09-13

Agradecimentos a @MarcAntoineCRUE e @irrwisch1 pelas contribuições e sugestões desta versão.

### Corrigido

- Adicionadas verificações abrangentes de segurança de buffers para evitar acesso fora dos limites da memória.
- Corrigido o tratamento de QoS 1 e QoS 2 no cliente, com processamento adequado das sequências PUBACK, PUBREC, PUBREL e PUBCOMP.
- Evitado possível bug em `setBufferSize()`.
- Diversas melhorias nos tipos usados na compilação.
- Substituído yield por delay em ambientes baseados em ESP32/FreeRTOS.

### Alterado

- Melhorada e ampliada a cobertura de testes para todos os níveis de QoS (0, 1 e 2).

## [3.3.0] - 2025-12-14

### Adicionado

- Acelerada a publicação de mensagens grandes usando um buffer, por @TD-er em [#59](https://github.com/hmueller01/pubsubclient3/pull/59).
- Suporte a tópicos em `PROGMEM` e `__FlashStringHelper`, por @hmueller01 em [#72](https://github.com/hmueller01/pubsubclient3/pull/72).
- Adicionado modelo de issue, por @hmueller01 em [#76](https://github.com/hmueller01/pubsubclient3/pull/76).
- Relato das diferenças de tamanho nos pull requests, por @hmueller01 em [#77](https://github.com/hmueller01/pubsubclient3/pull/77).

### Alterado

- Refatorados membros privados, por @hmueller01 em [#68](https://github.com/hmueller01/pubsubclient3/pull/68).
- Refatoradas funções privadas, por @hmueller01 em [#70](https://github.com/hmueller01/pubsubclient3/pull/70).
- Evitado acesso inválido quando `_client` não foi configurado, por @hmueller01 em [#69](https://github.com/hmueller01/pubsubclient3/pull/69).
- Pequenas atualizações, por @hmueller01 em [#73](https://github.com/hmueller01/pubsubclient3/pull/73).
- Otimização com inline, por @hmueller01 em [#75](https://github.com/hmueller01/pubsubclient3/pull/75).
- Atualizado actions/upload-pages-artifact de 3 para 4, por @dependabot[bot] em [#78](https://github.com/hmueller01/pubsubclient3/pull/78).
- Atualizado actions/checkout de 2 para 5, por @dependabot[bot] em [#79](https://github.com/hmueller01/pubsubclient3/pull/79).
- Marcado parâmetro não utilizado para evitar avisos, por @hmueller01 em [#82](https://github.com/hmueller01/pubsubclient3/pull/82).
- Corrigido aviso de compilação no limite de strnlen, por @hmueller01 em [#83](https://github.com/hmueller01/pubsubclient3/pull/83).
- Corrigido uso obsoleto de `BUILTIN_LED`, por @hmueller01 em [#84](https://github.com/hmueller01/pubsubclient3/pull/84).
- Atualizado actions/checkout de 5 para 6, por @dependabot[bot] em [#85](https://github.com/hmueller01/pubsubclient3/pull/85).
- Atualizados workflows, por @hmueller01 em [#86](https://github.com/hmueller01/pubsubclient3/pull/86).

## [3.2.1] - 2025-09-11

### Corrigido

- Corrigido bug introduzido pelo suporte a QoS &gt; 0 (#65).

## [3.2.0] - 2025-07-20

### Adicionado

- Suporte à publicação com QoS 1 e 2, por @hmueller01 (#49).
- Documentação da API com Doxygen em gh-pages, por @hmueller01 (#51).

### Alterado

- Adicionada verificação de nullptr em `CHECK_STRING_LENGTH`, por @TD-er (#48).
- Movido `CHECK_STRING_LENGTH` para `PubSubClient.cpp`, por @hmueller01 (#50).
- Removido parâmetro de chamada adiada (lazy calling), por @hmueller01 (#53).
- Implementado `writeNextMsgId()`, por @hmueller01 (#54).
- Simplificado `writeString()`, por @hmueller01 (#55).

## [3.1.0] - 2025-03-20

### Adicionado

- Adicionadas saídas `ERROR_PSC_PRINTF` e `ERROR_PSC_PRINTF_P` (#25).

### Alterado

- Corrigido `setServer(domain, ...)` copiando a string do domínio para um buffer interno, evitando ponteiro potencialmente inválido (#10).
- Conexão ao broker somente se a porta for diferente de zero, protegendo contra inicialização incorreta (#10).
- Refatorados os tipos de tamanho de buffer para `size_t` (#11).
- Adicionados avisos e `-Werror` ao Makefile de testes (#11).
- Reformatado o código dos testes com o estilo Google (veja `.clang-format`) (#11).
- Corrigido tratamento de keepalive; agradecimentos a @uschnindler (#14).
- Utilizadas listas de inicialização nos construtores (#15).
- Corrigidas saídas de `DEBUG_PSC_PRINTF` (#18).
- Introduzido indicador `MQTTRETAINED` (#21).
- Adicionada documentação de `readByte()` e `readPacket()` (#23).
- Refatorados `buildHeader()`, `readPacket()`, `readByte()`, `beginPublish()`/`endPublish()`, `publish()`/`publish_P()`, `connect()`, `connected()`, `subscribe()`/`unsubscribe()` e o controle interno de tempos (#22, #23, #28, #30, #31, #33, #34, #37, #38, #39, #40, #44).
- Corrigida possível corrupção de memória na preparação do callback de `MQTTPUBLISH` (#25).
- Adicionada verificação de falha ausente em `_client->read()` dentro de `readByte()` (#32).
- Substituídas listas de inicialização dos construtores pela inicialização dos membros na classe, conforme diretriz C.45 do C++ (#42).

## [3.0.2] - 2025-02-27

### Adicionado

- Workflow do GitHub para executar testes.
- Workflow do GitHub para compilar exemplos usando Arduino CLI.
- Exemplos: utilizada uma fonte melhor de aleatoriedade (#9).
- Opção para desativar o uso de `std::function` (#7).

## [3.0.1] - 2025-02-20

### Adicionado

- Adicionado `.editorconfig`.
- Adicionado workflow do GitHub para verificar o estilo do código em push/PR.

### Alterado

- Reformatado o código com o estilo Google.
- Correções dos testes: definição ausente de `strlen_P` e bibliotecas Python.

## [3.0] - 2025-02-17

Manutenção assumida por Holger Mueller.

### Adicionado

- Adicionada documentação no código do cabeçalho de `PubSubClient`.
- Adicionada opção para habilitar depuração da biblioteca.

### Alterado

- Uso obrigatório do construtor `PubSubClient()`.
- Utilizado `bool` no lugar de `boolean`.
- Adicionadas chamadas de `yield()` em `connect()` e `write()` para evitar resets do watchdog.
- Corrigido bug no tratamento do tamanho do payload em PROGMEM em `publish_P`.
- Ampliado `bytesToWrite` para `uint16_t` para evitar overflow.
- Removido aviso do compilador sobre `expectedLength` sem sinal.
- Ampliado o uso de `std::function` onde disponível.
- Corrigido o caso de keepalive igual a zero.

## [2.8] - 2020-05-20

### Adicionado

- `setBufferSize()` para substituir `MQTT_MAX_PACKET_SIZE`.
- `setKeepAlive()` para substituir `MQTT_KEEPALIVE`.
- `setSocketTimeout()` para substituir `MQTT_SOCKET_TIMEOUT`.
- Verificação para impedir subscribe/unsubscribe de tópicos vazios.
- Declaração do modo WiFi antes da conexão no exemplo ESP.
- Uso de `strnlen` para evitar acesso fora dos limites.
- Suporte a objetos `Client` já conectados.

## [2.7] - 2018-11-02

### Adicionado

- API para payloads grandes: `beginPublish`/`write`/`publish`/`endPublish`.
- Adicionadas chamadas de `yield` para melhorar a confiabilidade no ESP.
- Adicionado indicador Clean Session às opções de conexão.
- Adicionado suporte a ESP32 para a assinatura de callback baseada em `std::function`.

### Corrigido

- Corrigido tratamento de Remaining Length para evitar escrita fora do buffer.

## [2.4] - 2015-11-21

### Adicionado

- `MQTT_SOCKET_TIMEOUT` para evitar bloqueio indefinido na espera por dados de entrada.

### Corrigido

- Corrigido código de retorno ao publicar mais de 256 bytes.

## [2.3] - 2015-09-11

### Adicionado

- Sobrecarga de conveniência `publish(topic,payload,retained)`.

## [2.2] - 2015-09-07

### Alterado

- Alterada a organização do código para atender aos requisitos de bibliotecas Arduino.

## [2.1] - 2015-09-07

### Adicionado

- `MAX_TRANSFER_SIZE` para dividir mensagens em blocos quando necessário.
- Rejeição de tópicos/payloads que excedam `MQTT_MAX_PACKET_SIZE`.

## [2.0] - 2015-08-28

### Adicionado

- Suporte a MQTT 3.1.1.
- Corrigido tratamento de PROGMEM para Intel Galileo/ESP8266.
- Sobrecargas de construtores por conveniência.
- Setters encadeáveis para servidor/callback/cliente/stream.
- Função `state()` para retornar o código CONNACK.

## [1.0] - 2009-02-02

- Notas da versão inicial: somente QoS 0, tamanho máximo de mensagem de 128 bytes, keepalive de 30 segundos e sem mensagens Will.
