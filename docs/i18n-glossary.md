# Glossary: the menus' terms in every language

Made by `tools/glossary.py` from the tables (`src/host/lang/*.inc`): change a term there, then run it again. A term is
the same wherever it appears (a page's name, a button, a help line). The games' own names (Virtua Fighter 3, Team
Battle, Revision D), the fighters', key and button names as printed (Start, Enter, A), "VS" and the ROM's own text
are never translated; a room without a name keeps "NAME'S ROOM" in English everywhere (everyone sees the same name).

| ID | what it is | en | ko | ja | zh-Hans | zh-Hant | es | pt-BR | fr | ru | de | it | pl | tr |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `P_ONLINE` | online play, the Online page | Online | 온라인 | オンライン | 联机 | 連線 | Online | Online | En ligne | Онлайн | Online | Online | Online | Çevrimiçi |
| `QUICK_MATCH_TAG` | matchmaking with a stranger near you (the Random of older versions) | QUICK MATCH | 퀵 매치 | クイックマッチ | 快速匹配 | 快速配對 | PARTIDA RÁPIDA | PARTIDA RÁPIDA | MATCH RAPIDE | БЫСТРЫЙ МАТЧ | SCHNELLES MATCH | PARTITA RAPIDA | SZYBKI MECZ | HIZLI MAÇ |
| `P_ROOM` | a room: a host waiting, players in line, watchers | Room | 방 | ルーム | 房间 | 房間 | Sala | Sala | Salon | Комната | Raum | Stanza | Pokój | Oda |
| `P_CREATE_ROOM` | opening a room | Create room | 방 만들기 | ルーム作成 | 创建房间 | 建立房間 | Crear sala | Criar sala | Créer un salon | Создать комнату | Raum erstellen | Crea stanza | Utwórz pokój | Oda kur |
| `LISTED_IN_ROOMS` | a room shown in the public list | Listed in Rooms | 방 목록에 공개 | ルーム一覧に公開 | 在房间列表公开 | 在房間列表公開 | Pública en Salas | Pública em Salas | Public dans les salons | В списке комнат | In Räumen gelistet | Pubblica in Stanze | Publiczny w Pokojach | Odalar'da listele |
| `P_ENTER_CODE` | typing a room's code | Enter code | 코드 입력 | コード入力 | 输入代码 | 輸入代碼 | Código | Código | Entrer un code | Код | Code eingeben | Inserisci codice | Wpisz kod | Kod gir |
| `COL_HOST` | the room's owner, player 1 | HOST | 방장 | ホスト | 房主 | 房主 | ANFITRIÓN | ANFITRIÃO | HÔTE | ХОСТ | HOST | HOST | HOST | KURUCU |
| `ROW_JOIN` | to play in a room | JOIN | 참가 | 参加 | 加入 | 加入 | ENTRAR | ENTRAR | REJOINDRE | ВОЙТИ | BEITRETEN | ENTRA | DOŁĄCZ | KATIL |
| `ROW_WATCH` | to spectate | WATCH | 관전 | 観戦 | 观战 | 觀戰 | VER | ASSISTIR | VOIR | СМОТРЕТЬ | ANSEHEN | GUARDA | OGLĄDAJ | İZLE |
| `COL_WATCHING` | spectators (a count) | WATCHING | 관전 | 観戦 | 观战 | 觀戰 | VIENDO | ASSISTINDO | SPECTATEURS | ЗРИТЕЛИ | ZUSCHAUER | SPETTATORI | WIDZOWIE | İZLEYEN |
| `PLAY_NEXT` | to wait in a room for your turn | Play next | 다음 대전 대기 | 次に対戦 | 排队对战 | 排隊對戰 | Jugar después | Jogar depois | Jouer ensuite | Играть следующим | Als Nächstes spielen | Gioca dopo | Zagraj następny | Sırada oyna |
| `BUTTON_PLAY_AGAIN` | a rematch | Play again | 재대전 | 再戦 | 再战 | 再戰 | Revancha | Revanche | Revanche | Реванш | Revanche | Rivincita | Rewanż | Tekrar oyna |
| `BUTTON_NEW_OPPONENT` | back to matchmaking | New opponent | 새 상대 | 別の相手 | 换个对手 | 換個對手 | Otro rival | Outro oponente | Autre adversaire | Другой соперник | Neuer Gegner | Nuovo avversario | Nowy rywal | Yeni rakip |
| `MR_LEAVE` | ending a match early | Leave the match | 대전 나가기 | 対戦をやめる | 离开对战 | 離開對戰 | Abandonar la partida | Sair da partida | Quitter le match | Покинуть матч | Match verlassen | Lascia l'incontro | Opuść mecz | Maçtan ayrıl |
| `OS_DELAY` | input delay (frames) | Input delay | 입력 지연 | 入力遅延 | 输入延迟 | 輸入延遲 | Retraso de entrada | Atraso de entrada | Délai d'entrée | Задержка ввода | Eingabeverzögerung | Ritardo input | Opóźnienie wejścia | Giriş gecikmesi |
| `CELL_RB` | rollback | RB | 롤백 | RB | 回滚 | 回滾 | RB | RB | RB | ОТК | RB | RB | RB | RB |
| `VS_RELAY` | the relay server (not a direct connection) | Connected through the relay | 릴레이로 연결 | リレー経由で接続 | 经中继连接 | 經中繼連線 | Conectado por el relay | Conectado pelo relay | Connecté par le relais | Через ретранслятор | Über das Relay verbunden | Connesso tramite relay | Połączono przez przekaźnik | Aktarıcı üzerinden bağlı |
| `VS_DIRECT` | a direct connection | Connected directly | 직접 연결 | 直接接続 | 直连 | 直連 | Conexión directa | Conexão direta | Connexion directe | Прямое соединение | Direkt verbunden | Connessione diretta | Połączenie bezpośrednie | Doğrudan bağlı |
| `OS_HIDE_IP` | a setting | Hide my IP address | 내 IP 숨기기 | IPアドレスを隠す | 隐藏我的 IP | 隱藏我的 IP | Ocultar mi IP | Ocultar meu IP | Masquer mon IP | Скрыть мой IP | Meine IP verbergen | Nascondi il mio IP | Ukryj moje IP | IP adresimi gizle |
| `OS_STAGE_SELECT` | the stage select on the arcade screen | Stage select | 스테이지 선택 | ステージセレクト | 场地选择 | 場地選擇 | Selección de escenario | Seleção de cenário | Choix du stage | Выбор арены | Stage-Wahl | Scelta stage | Wybór areny | Sahne seçimi |
| `A_CHALLENGER` | the test menu's value: the challenger picks | Challenger | 챌린저 | 挑戦者 | 乱入方 | 亂入方 | Retador | Desafiante | Challenger | Challenger | Challenger | Sfidante | Challenger | Challenger |
| `C_DURAL` | the boss, a cheat | Dural on the select | 셀렉트에 듀랄 | セレクトにデュラル | 选人画面加入 Dural | 選人畫面加入 Dural | Dural en la selección | Dural na seleção | Dural à la sélection | Dural в выборе | Dural in der Auswahl | Dural nella selezione | Dural w wyborze postaci | Seçim ekranında Dural |
| `C_DURAL_STAGE` | the boss's stage | Dural's stage | 듀랄 스테이지 | デュラルのステージ | Dural 的场地 | Dural 的場地 | Escenario de Dural | Cenário da Dural | Stage de Dural | Арена Dural | Durals Stage | Stage di Dural | Arena Dural | Dural'ın sahnesi |
| `P_SETTINGS` | settings | Settings | 설정 | 設定 | 设置 | 設定 | Ajustes | Ajustes | Réglages | Настройки | Einstellungen | Impostazioni | Ustawienia | Ayarlar |
| `P_DISPLAY` | the Display page | Display | 화면 | 画面 | 画面 | 畫面 | Pantalla | Tela | Affichage | Экран | Anzeige | Schermo | Obraz | Görüntü |
| `D_SCALE` | render scale | Render scale | 렌더링 배율 | 描画倍率 | 渲染倍率 | 渲染倍率 | Escala de render | Escala de render | Échelle de rendu | Масштаб рендера | Render-Skalierung | Scala di rendering | Skala renderowania | Görüntü ölçeği |
| `D_AA` | antialiasing | Antialiasing | 안티에일리어싱 | アンチエイリアス | 抗锯齿 | 抗鋸齒 | Antialiasing | Antialiasing | Anticrénelage | Сглаживание | Kantenglättung | Antialiasing | Wygładzanie | Kenar yumuşatma |
| `D_RENDERER` | the 3D renderer (GPU, CPU) | 3D renderer | 3D 렌더러 | 3Dレンダラー | 3D 渲染器 | 3D 渲染器 | Renderizador 3D | Renderizador 3D | Rendu 3D | 3D-рендер | 3D-Renderer | Renderer 3D | Renderer 3D | 3B işleyici |
| `D_FULLSCREEN` | fullscreen | Fullscreen | 전체 화면 | フルスクリーン | 全屏 | 全螢幕 | Pantalla completa | Tela cheia | Plein écran | Полный экран | Vollbild | Schermo intero | Pełny ekran | Tam ekran |
| `D_SCREEN_FILTER` | scanlines, CRT | Screen filter | 화면 필터 | 画面フィルター | 画面滤镜 | 畫面濾鏡 | Filtro de pantalla | Filtro de tela | Filtre d'écran | Фильтр экрана | Bildfilter | Filtro schermo | Filtr ekranu | Ekran filtresi |
| `P_SOUND` | the Sound page | Sound | 사운드 | サウンド | 声音 | 聲音 | Sonido | Som | Son | Звук | Ton | Audio | Dźwięk | Ses |
| `P_GAME` | the Game page | Game | 게임 | ゲーム | 游戏 | 遊戲 | Juego | Jogo | Jeu | Игра | Spiel | Gioco | Gra | Oyun |
| `G_LANGUAGE` | the language setting | Language | 언어 | 言語 | 语言 | 語言 | Idioma | Idioma | Langue | Язык | Sprache | Lingua | Język | Dil |
| `G_PLAY_STYLE` | PC (free play) or Arcade (coins) | Play style | 플레이 방식 | プレイスタイル | 游玩方式 | 遊玩方式 | Modo de juego | Estilo de jogo | Style de jeu | Режим игры | Spielweise | Stile di gioco | Styl gry | Oyun tarzı |
| `G_ARCADE` | the arcade way | Arcade | 아케이드 | アーケード | 街机 | 街機 | Arcade | Arcade | Arcade | Аркада | Automat | Arcade | Automat | Salon |
| `M_ARCADE` | the arcade's own settings | Arcade settings | 아케이드 설정 | アーケード設定 | 街机设置 | 街機設定 | Ajustes arcade | Ajustes do arcade | Réglages arcade | Настройки автомата | Automat | Impostazioni arcade | Ustawienia automatu | Salon ayarları |
| `G_VERSION` | Revision D or Team Battle | Version | 버전 | バージョン | 版本 | 版本 | Versión | Versão | Version | Версия | Version | Versione | Wersja | Sürüm |
| `G_TB_MODE` | Team Battle's 1 vs 1 or 3 vs 3 | Team Battle mode | 팀 배틀 모드 | チームバトルのモード | Team Battle 模式 | Team Battle 模式 | Modo Team Battle | Modo Team Battle | Mode Team Battle | Режим Team Battle | Team-Battle-Modus | Modalità Team Battle | Tryb Team Battle | Team Battle modu |
| `G_3V3` | three fighters a side | 3 vs 3 | 3대3 | 3対3 | 3 对 3 | 3 對 3 | 3 vs 3 | 3 vs 3 | 3 contre 3 | 3 на 3 | 3 gegen 3 | 3 contro 3 | 3 na 3 | 3'e 3 |
| `A_DIFFICULTY` | the CPU's difficulty | Difficulty | 난이도 | 難易度 | 难度 | 難度 | Dificultad | Dificuldade | Difficulté | Сложность | Schwierigkeit | Difficoltà | Poziom trudności | Zorluk |
| `A_ROUNDS_VS` | rounds to win a match | Rounds to win (vs player) | 승리 라운드 (대인전) | 勝利ラウンド数（対人） | 胜利局数（对人） | 勝利局數（對人） | Rondas para ganar (vs jugador) | Rounds para vencer (vs jogador) | Rounds pour gagner (vs joueur) | Раундов для победы (игрок) | Siegrunden (vs Spieler) | Round per vincere (vs giocatore) | Rundy do wygranej (gracz) | Kazanma rauntu (oyuncuya karşı) |
| `A_ENERGY_VS` | the health bar ("energy" in the arcade's own words) | Energy (vs player) | 체력 (대인전) | 体力（対人） | 体力（对人） | 體力（對人） | Energía (vs jugador) | Energia (vs jogador) | Énergie (vs joueur) | Здоровье (игрок) | Energie (vs Spieler) | Energia (vs giocatore) | Energia (gracz) | Enerji (oyuncuya karşı) |
| `P_CHEATS` | cheats | Cheats | 치트 | チート | 作弊 | 作弊 | Trucos | Trapaças | Astuces | Читы | Cheats | Trucchi | Kody | Hileler |
| `P_CONTROLS` | controls | Controls | 조작 | 操作 | 操作 | 操作 | Controles | Controles | Commandes | Управление | Steuerung | Comandi | Sterowanie | Kontroller |
| `CTL_PAD` | a gamepad | Pad | 패드 | パッド | 手柄 | 手把 | Mando | Controle | Manette | Геймпад | Controller | Controller | Pad | Kol |
| `CTL_GUARD` | a button: Guard | Guard | 가드 | ガード | 防御 | 防禦 | Defensa | Defesa | Garde | Блок | Block | Parata | Blok | Guard |
| `CTL_PUNCH` | a button: Punch | Punch | 펀치 | パンチ | 拳 | 拳 | Puñetazo | Soco | Poing | Рука | Schlag | Pugno | Cios | Punch |
| `CTL_KICK` | a button: Kick | Kick | 킥 | キック | 脚 | 腳 | Patada | Chute | Pied | Нога | Tritt | Calcio | Kopnięcie | Kick |
| `CTL_ESCAPE` | the evade (VF3's E button) | Escape | 이스케이프 | エスケープ | 闪避 | 閃避 | Esquivar | Esquiva | Esquive | Уклон | Ausweichen | Schivata | Unik | Kaçış |
| `CTL_COIN` | a credit | Coin | 코인 | コイン | 投币 | 投幣 | Moneda | Ficha | Pièce | Монета | Münze | Gettone | Moneta | Jeton |
| `CTL_SETUP` | mapping a pad's buttons | Set up buttons | 버튼 설정 | ボタン設定 | 设定按钮 | 設定按鈕 | Configurar botones | Configurar botões | Configurer les boutons | Настроить кнопки | Knöpfe einrichten | Configura pulsanti | Skonfiguruj przyciski | Düğmeleri ayarla |
| `TITLE_PAUSED` | the menu's title | PAUSED | 일시 정지 | ポーズ | 暂停 | 暫停 | PAUSA | PAUSADO | PAUSE | ПАУЗА | PAUSE | PAUSA | PAUZA | DURAKLATILDI |
| `TITLE_ONLINE_MATCH` | the menu's title in a match | ONLINE MATCH | 온라인 대전 | オンライン対戦 | 联机对战 | 連線對戰 | PARTIDA ONLINE | PARTIDA ONLINE | MATCH EN LIGNE | ОНЛАЙН-МАТЧ | ONLINE-MATCH | INCONTRO ONLINE | MECZ ONLINE | ÇEVRİMİÇİ MAÇ |
| `WATCHING` | spectating (the title) | WATCHING | 관전 중 | 観戦中 | 观战中 | 觀戰中 | VIENDO | ASSISTINDO | EN SPECTATEUR | ПРОСМОТР | ZUSCHAUEN | SPETTATORE | OGLĄDANIE | İZLENİYOR |
| `MATCH_OVER` | the end of a match | MATCH OVER | 대전 종료 | 対戦終了 | 对战结束 | 對戰結束 | FIN DE LA PARTIDA | FIM DA PARTIDA | MATCH TERMINÉ | МАТЧ ОКОНЧЕН | MATCH VORBEI | INCONTRO FINITO | KONIEC MECZU | MAÇ BİTTİ |
| `CONNECTION_LOST` | a dropped connection | CONNECTION LOST | 연결 끊김 | 接続が切れました | 连接断开 | 連線斷開 | CONEXIÓN PERDIDA | CONEXÃO PERDIDA | CONNEXION PERDUE | СВЯЗЬ ПОТЕРЯНА | VERBINDUNG WEG | CONNESSIONE PERSA | UTRACONO POŁĄCZENIE | BAĞLANTI KOPTU |
| `FINDING_OPPONENT` | matchmaking | FINDING AN OPPONENT | 상대를 찾는 중 | 対戦相手を探しています | 寻找对手 | 尋找對手 | BUSCANDO RIVAL | PROCURANDO OPONENTE | RECHERCHE D'ADVERSAIRE | ИЩЕМ СОПЕРНИКА | SUCHE GEGNER | CERCO UN AVVERSARIO | SZUKAM RYWALA | RAKİP ARANIYOR |
| `M_RESUME` | back to the game | Resume | 계속하기 | 再開 | 继续 | 繼續 | Continuar | Continuar | Reprendre | Продолжить | Weiter | Riprendi | Wznów | Devam et |
| `BACK` | back a page | Back | 뒤로 | 戻る | 返回 | 返回 | Volver | Voltar | Retour | Назад | Zurück | Indietro | Wstecz | Geri |
| `M_QUIT` | closing the game | Quit | 종료 | 終了 | 退出 | 退出 | Salir | Sair | Quitter | Выход | Beenden | Esci | Wyjdź | Çıkış |
| `ON` | a setting on | On | 켜짐 | オン | 开 | 開 | Sí | Sim | Oui | Вкл. | An | Sì | Wł. | Açık |
| `OFF` | a setting off | Off | 꺼짐 | オフ | 关 | 關 | No | Não | Non | Выкл. | Aus | No | Wył. | Kapalı |
| `AUTO` | automatic | Auto | 자동 | 自動 | 自动 | 自動 | Auto | Auto | Auto | Авто | Auto | Auto | Auto | Otomatik |
