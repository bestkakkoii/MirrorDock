// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module uxplay_ui_text;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        constexpr std::size_t language_count = static_cast<std::size_t>(ui_language::count);

        // one text per language, in the order of ui_language
        struct text_row
        {
            ui_text id;
            std::array<std::wstring_view, language_count> text;
        };

        // per language (order of ui_language): the name for the log and for
        // "-ui", and the DirectWrite locale that selects its fonts
        struct language_names
        {
            std::string_view code;
            std::wstring_view locale;
        };
        constexpr std::array<language_names, language_count> languages{{
            {"zh-Hant", L"zh-TW"},
            {"zh-Hans", L"zh-CN"},
            {"en", L"en-US"},
            {"ja", L"ja-JP"},
            {"ko", L"ko-KR"},
            {"es", L"es-ES"},
            {"de", L"de-DE"},
            {"fr", L"fr-FR"},
            {"vi", L"vi-VN"},
            {"th", L"th-TH"},
            {"ru", L"ru-RU"},
        }};

        constexpr std::size_t english_index = static_cast<std::size_t>(ui_language::english);

        constexpr std::array<text_row, static_cast<std::size_t>(ui_text::count)> text_table{{
            // texts per row, in the order of ui_language:
            // zh-Hant, zh-Hans, en, ja, ko, es, de, fr, vi, th, ru
            // window and tray status
            {ui_text::waiting_for_connection,
             {L"等待連線",
              L"等待连接",
              L"Waiting for connection",
              L"接続待機中",
              L"연결 대기 중",
              L"Esperando conexión",
              L"Warten auf Verbindung",
              L"En attente de connexion",
              L"Đang chờ kết nối",
              L"รอการเชื่อมต่อ",
              L"Ожидание подключения"}},
            {ui_text::usb_checking,
             {L"USB：檢查中…",
              L"USB：正在检查…",
              L"USB: checking…",
              L"USB: 確認中…",
              L"USB: 확인 중…",
              L"USB: comprobando…",
              L"USB: wird geprüft…",
              L"USB : vérification…",
              L"USB: đang kiểm tra…",
              L"USB: กำลังตรวจสอบ…",
              L"USB: проверка…"}},
            {ui_text::usb_charging_only_status,
             {L"USB：僅充電",
              L"USB：仅充电",
              L"USB: charging only",
              L"USB: 充電のみ",
              L"USB: 충전 전용",
              L"USB: solo carga",
              L"USB: nur Laden",
              L"USB : charge uniquement",
              L"USB: chỉ sạc",
              L"USB: ชาร์จอย่างเดียว",
              L"USB: только зарядка"}},
            {ui_text::usb_not_connected,
             {L"USB：未接上",
              L"USB：未接入",
              L"USB: not plugged in",
              L"USB: 未接続",
              L"USB: 연결되지 않음",
              L"USB: no conectado",
              L"USB: nicht angeschlossen",
              L"USB : non branché",
              L"USB: chưa cắm cáp",
              L"USB: ยังไม่ได้เสียบสาย",
              L"USB: не подключено"}},
            {ui_text::usb_adapter_disabled,
             {L"USB：網路卡已停用",
              L"USB：网卡已禁用",
              L"USB: network adapter disabled",
              L"USB: ネットワークアダプターが無効",
              L"USB: 네트워크 어댑터 비활성화됨",
              L"USB: adaptador de red deshabilitado",
              L"USB: Netzwerkadapter deaktiviert",
              L"USB : carte réseau désactivée",
              L"USB: bộ điều hợp mạng đã bị tắt",
              L"USB: อะแดปเตอร์เครือข่ายถูกปิดใช้งาน",
              L"USB: сетевой адаптер отключен"}},
            {ui_text::usb_ready_at,
             {L"USB：已就緒",
              L"USB：已就绪",
              L"USB: ready",
              L"USB: 準備完了",
              L"USB: 준비 완료",
              L"USB: listo",
              L"USB: bereit",
              L"USB : prêt",
              L"USB: sẵn sàng",
              L"USB: พร้อมใช้งาน",
              L"USB: готово"}},
            {ui_text::usb_turn_on_hotspot,
             {L"USB：請開啟個人熱點",
              L"USB：请打开个人热点",
              L"USB: turn on Personal Hotspot",
              L"USB: インターネット共有をオンにしてください",
              L"USB: 개인용 핫스팟을 켜세요",
              L"USB: active Punto de acceso personal",
              L"USB: persönlichen Hotspot aktivieren",
              L"USB : activez le partage de connexion",
              L"USB: hãy bật Điểm truy cập cá nhân",
              L"USB: โปรดเปิดฮอตสปอตส่วนบุคคล",
              L"USB: включите «Режим модема»"}},
            // control bar and tray menu
            {ui_text::prefer_usb_link,
             {L"USB 優先",
              L"USB 优先",
              L"Prefer USB",
              L"USB 優先",
              L"USB 우선",
              L"Preferir USB",
              L"USB bevorzugen",
              L"USB en priorité",
              L"Ưu tiên USB",
              L"ใช้ USB เป็นหลัก",
              L"Приоритет USB"}},
            {ui_text::wifi_only,
             {L"僅 Wi-Fi",
              L"仅 Wi-Fi",
              L"Wi-Fi only",
              L"Wi-Fi のみ",
              L"Wi-Fi 전용",
              L"Solo Wi-Fi",
              L"Nur Wi-Fi",
              L"Wi-Fi uniquement",
              L"Chỉ Wi-Fi",
              L"Wi-Fi เท่านั้น",
              L"Только Wi-Fi"}},
            {ui_text::protect_mobile_data,
             {L"省行動數據",
              L"节省手机流量",
              L"Save mobile data",
              L"データ通信量を節約",
              L"셀룰러 데이터 절약",
              L"Ahorrar datos móviles",
              L"Mobile Daten sparen",
              L"Économiser les données mobiles",
              L"Tiết kiệm dữ liệu di động",
              L"ประหยัดเน็ตมือถือ",
              L"Экономить трафик"}},
            {ui_text::protect_mobile_data_menu,
             {L"省行動數據…",
              L"节省手机流量…",
              L"Save mobile data…",
              L"データ通信量を節約…",
              L"셀룰러 데이터 절약…",
              L"Ahorrar datos móviles…",
              L"Mobile Daten sparen…",
              L"Économiser les données mobiles…",
              L"Tiết kiệm dữ liệu di động…",
              L"ประหยัดเน็ตมือถือ…",
              L"Экономить трафик…"}},
            {ui_text::enable_usb_adapter,
             {L"啟用 USB 網路卡",
              L"启用 USB 网卡",
              L"Enable USB adapter",
              L"USB アダプターを有効化",
              L"USB 어댑터 활성화",
              L"Habilitar adaptador USB",
              L"USB-Adapter aktivieren",
              L"Activer la carte réseau USB",
              L"Bật bộ điều hợp USB",
              L"เปิดอะแดปเตอร์ USB",
              L"Включить USB-адаптер"}},
            {ui_text::enable_usb_adapter_menu,
             {L"啟用 USB 網路卡…",
              L"启用 USB 网卡…",
              L"Enable USB network adapter…",
              L"USB ネットワークアダプターを有効にする…",
              L"USB 네트워크 어댑터 활성화…",
              L"Habilitar adaptador de red USB…",
              L"USB-Netzwerkadapter aktivieren…",
              L"Activer la carte réseau USB…",
              L"Bật bộ điều hợp mạng USB…",
              L"เปิดใช้งานอะแดปเตอร์เครือข่าย USB…",
              L"Включить сетевой адаптер USB…"}},
            {ui_text::full_screen,
             {L"全螢幕",
              L"全屏",
              L"Full screen",
              L"全画面表示",
              L"전체 화면",
              L"Pantalla completa",
              L"Vollbild",
              L"Plein écran",
              L"Toàn màn hình",
              L"เต็มหน้าจอ",
              L"Во весь экран"}},
            {ui_text::exit_full_screen,
             {L"結束全螢幕",
              L"退出全屏",
              L"Exit full screen",
              L"全画面表示を終了",
              L"전체 화면 종료",
              L"Salir de pantalla completa",
              L"Vollbild beenden",
              L"Quitter le plein écran",
              L"Thoát toàn màn hình",
              L"ออกจากโหมดเต็มหน้าจอ",
              L"Выйти из полноэкранного режима"}},
            {ui_text::more,
             {L"更多",
              L"更多",
              L"More",
              L"その他",
              L"더 보기",
              L"Más",
              L"Mehr",
              L"Plus",
              L"Tùy chọn khác",
              L"เพิ่มเติม",
              L"Еще"}},
            {ui_text::mute,
             {L"靜音",
              L"静音",
              L"Mute",
              L"ミュート",
              L"음소거",
              L"Silenciar",
              L"Stummschalten",
              L"Couper le son",
              L"Tắt tiếng",
              L"ปิดเสียง",
              L"Выключить звук"}},
            {ui_text::unmute,
             {L"取消靜音",
              L"取消静音",
              L"Unmute",
              L"ミュート解除",
              L"음소거 해제",
              L"Activar sonido",
              L"Ton einschalten",
              L"Réactiver le son",
              L"Bật tiếng",
              L"เปิดเสียง",
              L"Включить звук"}},
            {ui_text::always_on_top,
             {L"最上層顯示",
              L"窗口置顶",
              L"Always on top",
              L"常に手前に表示",
              L"항상 위에 표시",
              L"Siempre visible",
              L"Immer im Vordergrund",
              L"Toujours au premier plan",
              L"Luôn ở trên cùng",
              L"อยู่บนสุดเสมอ",
              L"Поверх всех окон"}},
            {ui_text::mode_quality,
             {L"畫質優先",
              L"画质优先",
              L"Quality",
              L"画質優先",
              L"화질 우선",
              L"Calidad",
              L"Qualität",
              L"Qualité",
              L"Chất lượng",
              L"เน้นคุณภาพ",
              L"Качество"}},
            {ui_text::mode_balanced,
             {L"平衡（自動調整）",
              L"均衡（自动调整）",
              L"Balanced (adaptive)",
              L"バランス（自動調整）",
              L"균형(자동 조정)",
              L"Equilibrado (adaptativo)",
              L"Ausgewogen (adaptiv)",
              L"Équilibré (adaptatif)",
              L"Cân bằng (tự điều chỉnh)",
              L"สมดุล (ปรับอัตโนมัติ)",
              L"Баланс (автоподстройка)"}},
            {ui_text::mode_performance,
             {L"效能全開",
              L"性能优先",
              L"Performance",
              L"パフォーマンス優先",
              L"성능 우선",
              L"Rendimiento",
              L"Leistung",
              L"Performances",
              L"Hiệu năng",
              L"เน้นประสิทธิภาพ",
              L"Производительность"}},
            {ui_text::start_with_windows,
             {L"開機時啟動",
              L"开机自动启动",
              L"Start with Windows",
              L"Windows 起動時に実行",
              L"Windows 시작 시 실행",
              L"Iniciar con Windows",
              L"Mit Windows starten",
              L"Lancer au démarrage de Windows",
              L"Khởi động cùng Windows",
              L"เริ่มทำงานพร้อม Windows",
              L"Запускать вместе с Windows"}},
            {ui_text::open_log,
             {L"開啟記錄檔",
              L"打开日志",
              L"Open log file",
              L"ログファイルを開く",
              L"로그 파일 열기",
              L"Abrir archivo de registro",
              L"Protokolldatei öffnen",
              L"Ouvrir le fichier journal",
              L"Mở tệp nhật ký",
              L"เปิดไฟล์บันทึก",
              L"Открыть файл журнала"}},
            {ui_text::about,
             {L"關於…",
              L"关于…",
              L"About…",
              L"バージョン情報…",
              L"프로그램 정보…",
              L"Acerca de…",
              L"Info…",
              L"À propos…",
              L"Giới thiệu…",
              L"เกี่ยวกับ…",
              L"О программе…"}},
            {ui_text::exit,
             {L"結束",
              L"退出",
              L"Exit",
              L"終了",
              L"종료",
              L"Salir",
              L"Beenden",
              L"Quitter",
              L"Thoát",
              L"ออก",
              L"Выход"}},
            // notifications: USB link
            {ui_text::data_guard_offer_title,
             {L"電腦正在使用 iPhone 的行動網路",
              L"电脑正在使用 iPhone 的手机流量",
              L"This PC is using the iPhone's mobile data",
              L"この PC が iPhone のモバイルデータ通信を使用しています",
              L"PC가 iPhone의 셀룰러 데이터를 사용하고 있습니다",
              L"Este equipo está usando los datos móviles del iPhone",
              L"Dieser PC nutzt die mobilen Daten des iPhone",
              L"Ce PC utilise les données mobiles de l’iPhone",
              L"Máy tính đang dùng dữ liệu di động của iPhone",
              L"คอมพิวเตอร์เครื่องนี้กำลังใช้เน็ตมือถือของ iPhone",
              L"Компьютер расходует мобильный трафик iPhone"}},
            {ui_text::data_guard_offer_text,
             {L"點此讓電腦上網不再經過 iPhone（需要系統管理員權限）。USB 鏡像不受影響。",
              L"点击此处让电脑上网不再经过 iPhone（需要管理员权限）。USB 镜像不受影响。",
              L"Click to keep this PC's internet off the iPhone (needs administrator permission). Mirroring over"
              L" USB is not affected.",
              L"クリックすると、PC のインターネット通信が iPhone を経由しないように設定します（管理者権限が必要です）。USB でのミラーリングには影響しません。",
              L"여기를 클릭하면 PC의 인터넷 연결이 iPhone을 거치지 않게 됩니다(관리자 권한 필요). USB 미러링에는 영향이 없습니다.",
              L"Haga clic para que la conexión a Internet de este equipo deje de pasar por el iPhone (requiere p"
              L"ermisos de administrador). La duplicación por USB no se ve afectada.",
              L"Klicken Sie hier, damit der PC nicht mehr über das iPhone ins Internet geht (Administratorrechte"
              L" erforderlich). Die Spiegelung über USB bleibt davon unberührt.",
              L"Cliquez ici pour que l’accès à Internet de ce PC ne passe plus par l’iPhone (droits d’administra"
              L"teur requis). La recopie de l’écran par USB n’est pas affectée.",
              L"Bấm vào đây để máy tính không truy cập Internet qua iPhone nữa (cần quyền quản trị viên). Việc p"
              L"hản chiếu qua USB không bị ảnh hưởng.",
              L"คลิกที่นี่เพื่อไม่ให้คอมพิวเตอร์ออกอินเทอร์เน็ตผ่าน iPhone (ต้องใช้สิทธิ์ผู้ดูแลระบบ) การสะท้อนภ"
              L"าพหน้าจอผ่าน USB ไม่ได้รับผลกระทบ",
              L"Нажмите, чтобы компьютер перестал выходить в интернет через iPhone (нужны права администратора)."
              L" На дублирование экрана по USB это не повлияет."}},
            {ui_text::adapter_disabled_title,
             {L"USB 網路卡已停用",
              L"USB 网卡已禁用",
              L"USB network adapter disabled",
              L"USB ネットワークアダプターが無効になっています",
              L"USB 네트워크 어댑터가 비활성화되어 있습니다",
              L"Adaptador de red USB deshabilitado",
              L"USB-Netzwerkadapter deaktiviert",
              L"Carte réseau USB désactivée",
              L"Bộ điều hợp mạng USB đã bị tắt",
              L"อะแดปเตอร์เครือข่าย USB ถูกปิดใช้งาน",
              L"Сетевой адаптер USB отключен"}},
            {ui_text::adapter_disabled_text,
             {L"點此重新啟用，才能經由 USB 鏡像。",
              L"点击此处重新启用，才能通过 USB 镜像。",
              L"Click to enable it again for mirroring over USB.",
              L"クリックして有効に戻すと、USB でミラーリングできるようになります。",
              L"USB로 미러링하려면 여기를 클릭해 다시 활성화하세요.",
              L"Haga clic para volver a habilitarlo y poder duplicar por USB.",
              L"Klicken Sie hier, um ihn für die Spiegelung über USB wieder zu aktivieren.",
              L"Cliquez ici pour la réactiver et recopier l’écran par USB.",
              L"Bấm vào đây để bật lại và phản chiếu qua USB.",
              L"คลิกที่นี่เพื่อเปิดใช้งานอีกครั้ง จึงจะสะท้อนภาพหน้าจอผ่าน USB ได้",
              L"Нажмите, чтобы снова включить его для дублирования экрана по USB."}},
            {ui_text::usb_ready_title,
             {L"USB 已就緒",
              L"USB 已就绪",
              L"USB ready",
              L"USB の準備ができました",
              L"USB 준비 완료",
              L"USB listo",
              L"USB bereit",
              L"USB prêt",
              L"USB đã sẵn sàng",
              L"USB พร้อมใช้งาน",
              L"USB-подключение готово"}},
            {ui_text::usb_ready_text,
             {L"螢幕鏡像將經由 USB 線傳輸。",
              L"屏幕镜像将通过 USB 线传输。",
              L"Screen mirroring will use the USB cable.",
              L"画面ミラーリングに USB ケーブルを使用します。",
              L"화면 미러링에 USB 케이블을 사용합니다.",
              L"La duplicación de pantalla usará el cable USB.",
              L"Die Bildschirmspiegelung läuft über das USB-Kabel.",
              L"La recopie de l’écran passera par le câble USB.",
              L"Màn hình sẽ được phản chiếu qua cáp USB.",
              L"การสะท้อนภาพหน้าจอจะใช้สาย USB",
              L"Дублирование экрана пойдет по USB-кабелю."}},
            {ui_text::usb_ready_text_in_session,
             {L"目前的鏡像仍走 Wi-Fi；在 iPhone 重新選擇後會改走 USB。",
              L"当前镜像仍走 Wi-Fi；在 iPhone 上重新选择后会改走 USB。",
              L"This session stays on Wi-Fi; choose the receiver again on the iPhone to use USB.",
              L"現在のミラーリングは Wi-Fi のまま続きます。USB に切り替えるには、iPhone で接続先を選び直してください。",
              L"진행 중인 미러링은 Wi-Fi로 계속됩니다. USB를 쓰려면 iPhone에서 이 PC를 다시 선택하세요.",
              L"Esta sesión sigue por Wi-Fi; vuelva a elegir el receptor en el iPhone para usar USB.",
              L"Diese Sitzung bleibt bei Wi-Fi. Wählen Sie den Empfänger auf dem iPhone erneut aus, um USB zu nu"
              L"tzen.",
              L"Cette session reste en Wi-Fi ; choisissez de nouveau le récepteur sur l’iPhone pour passer en US"
              L"B.",
              L"Phiên này vẫn dùng Wi-Fi; hãy chọn lại máy tính trên iPhone để chuyển sang USB.",
              L"การสะท้อนภาพครั้งนี้ยังใช้ Wi-Fi ต่อไป หากต้องการใช้ USB ให้เลือกคอมพิวเตอร์เครื่องนี้อีกครั้งบน"
              L" iPhone",
              L"Текущий сеанс останется на Wi-Fi. Чтобы перейти на USB, выберите этот компьютер на iPhone заново"
              L"."}},
            {ui_text::iphone_attached_title,
             {L"iPhone 已接上",
              L"iPhone 已接入",
              L"iPhone plugged in",
              L"iPhone が接続されました",
              L"iPhone이 연결되었습니다",
              L"iPhone conectado",
              L"iPhone angeschlossen",
              L"iPhone branché",
              L"Đã cắm iPhone",
              L"เสียบสาย iPhone แล้ว",
              L"iPhone подключен"}},
            {ui_text::turn_on_hotspot_text,
             {L"開啟「個人熱點」即可經由 USB 鏡像。",
              L"打开“个人热点”即可通过 USB 镜像。",
              L"Turn on Personal Hotspot to mirror over USB.",
              L"「インターネット共有」をオンにすると、USB でミラーリングできます。",
              L"USB로 미러링하려면 ‘개인용 핫스팟’을 켜세요.",
              L"Active «Punto de acceso personal» (o «Compartir Internet») para duplicar por USB.",
              L"Aktivieren Sie den persönlichen Hotspot, um über USB zu spiegeln.",
              L"Activez « Partage de connexion » pour recopier l’écran par USB.",
              L"Hãy bật Điểm truy cập cá nhân để phản chiếu qua USB.",
              L"เปิด “ฮอตสปอตส่วนบุคคล” เพื่อสะท้อนภาพหน้าจอผ่าน USB",
              L"Чтобы дублировать экран по USB, включите «Режим модема»."}},
            {ui_text::adapter_enabled_title,
             {L"USB 網路卡已啟用",
              L"USB 网卡已启用",
              L"USB network adapter enabled",
              L"USB ネットワークアダプターを有効にしました",
              L"USB 네트워크 어댑터를 활성화했습니다",
              L"Adaptador de red USB habilitado",
              L"USB-Netzwerkadapter aktiviert",
              L"Carte réseau USB activée",
              L"Đã bật bộ điều hợp mạng USB",
              L"เปิดใช้งานอะแดปเตอร์เครือข่าย USB แล้ว",
              L"Сетевой адаптер USB включен"}},
            {ui_text::adapter_still_disabled_title,
             {L"USB 網路卡仍為停用狀態",
              L"USB 网卡仍处于禁用状态",
              L"USB network adapter is still disabled",
              L"USB ネットワークアダプターは無効のままです",
              L"USB 네트워크 어댑터가 아직 비활성화되어 있습니다",
              L"El adaptador de red USB sigue deshabilitado",
              L"USB-Netzwerkadapter ist weiterhin deaktiviert",
              L"La carte réseau USB est toujours désactivée",
              L"Bộ điều hợp mạng USB vẫn đang tắt",
              L"อะแดปเตอร์เครือข่าย USB ยังถูกปิดใช้งานอยู่",
              L"Сетевой адаптер USB по-прежнему отключен"}},
            {ui_text::admin_declined,
             {L"已取消系統管理員授權。",
              L"已取消管理员授权。",
              L"Administrator permission was declined.",
              L"管理者権限が許可されませんでした。",
              L"관리자 권한 요청이 거부되었습니다.",
              L"Se rechazó el permiso de administrador.",
              L"Die Administratorrechte wurden nicht erteilt.",
              L"L’autorisation d’administrateur a été refusée.",
              L"Yêu cầu cấp quyền quản trị viên đã bị từ chối.",
              L"ไม่ได้รับอนุญาตให้ใช้สิทธิ์ผู้ดูแลระบบ",
              L"Запрос прав администратора отклонен."}},
            {ui_text::enable_failed,
             {L"啟用失敗，詳見記錄檔。",
              L"启用失败，详见日志。",
              L"Could not enable it; see the log file.",
              L"有効にできませんでした。ログファイルを確認してください。",
              L"활성화하지 못했습니다. 로그 파일을 확인하세요.",
              L"No se pudo habilitar; consulte el archivo de registro.",
              L"Aktivieren fehlgeschlagen. Details stehen in der Protokolldatei.",
              L"Impossible de l’activer ; consultez le fichier journal.",
              L"Không bật được; xem chi tiết trong tệp nhật ký.",
              L"เปิดใช้งานไม่สำเร็จ โปรดดูไฟล์บันทึก",
              L"Не удалось включить адаптер. Подробности — в файле журнала."}},
            {ui_text::data_guard_on_title,
             {L"電腦上網已不再經過 iPhone",
              L"电脑上网已不再经过 iPhone",
              L"This PC no longer uses the iPhone for internet",
              L"PC のインターネット通信は iPhone を経由しなくなりました",
              L"PC가 더 이상 iPhone을 통해 인터넷에 연결하지 않습니다",
              L"Este equipo ya no usa Internet a través del iPhone",
              L"Dieser PC nutzt das iPhone nicht mehr fürs Internet",
              L"Ce PC n’accède plus à Internet via l’iPhone",
              L"Máy tính không còn truy cập Internet qua iPhone",
              L"คอมพิวเตอร์ไม่ออกอินเทอร์เน็ตผ่าน iPhone แล้ว",
              L"Компьютер больше не выходит в интернет через iPhone"}},
            {ui_text::data_guard_on_text,
             {L"USB 線仍可用於鏡像。",
              L"USB 线仍可用于镜像。",
              L"The USB cable still carries mirroring.",
              L"USB ケーブルは引き続きミラーリングに使用されます。",
              L"USB 케이블은 계속 미러링에 사용됩니다.",
              L"El cable USB sigue sirviendo para la duplicación.",
              L"Das USB-Kabel überträgt weiterhin die Spiegelung.",
              L"Le câble USB sert toujours à la recopie de l’écran.",
              L"Cáp USB vẫn được dùng để phản chiếu.",
              L"สาย USB ยังใช้สะท้อนภาพหน้าจอได้ตามเดิม",
              L"Дублирование экрана по USB-кабелю продолжает работать."}},
            {ui_text::data_guard_off_title,
             {L"未能變更電腦的上網路徑",
              L"未能更改电脑的上网路径",
              L"The PC's internet route was not changed",
              L"PC のインターネット経路は変更されませんでした",
              L"PC의 인터넷 경로가 변경되지 않았습니다",
              L"No se cambió la ruta de Internet del equipo",
              L"Die Internetroute des PCs wurde nicht geändert",
              L"L’accès à Internet du PC n’a pas été modifié",
              L"Đường truyền Internet của máy tính chưa được thay đổi",
              L"ไม่ได้เปลี่ยนเส้นทางอินเทอร์เน็ตของคอมพิวเตอร์",
              L"Маршрут выхода в интернет не изменен"}},
            {ui_text::apply_failed,
             {L"設定失敗，詳見記錄檔。",
              L"设置失败，详见日志。",
              L"Could not apply it; see the log file.",
              L"適用できませんでした。ログファイルを確認してください。",
              L"적용하지 못했습니다. 로그 파일을 확인하세요.",
              L"No se pudo aplicar; consulte el archivo de registro.",
              L"Übernehmen fehlgeschlagen. Details stehen in der Protokolldatei.",
              L"Impossible d’appliquer le réglage ; consultez le fichier journal.",
              L"Không áp dụng được; xem chi tiết trong tệp nhật ký.",
              L"ตั้งค่าไม่สำเร็จ โปรดดูไฟล์บันทึก",
              L"Не удалось применить изменение. Подробности — в файле журнала."}},
            {ui_text::switched_to_usb_first,
             {L"已切換為 USB 優先",
              L"已切换为 USB 优先",
              L"Switched to Prefer USB",
              L"「USB 優先」に切り替えました",
              L"‘USB 우선’으로 전환했습니다",
              L"Se cambió a «Preferir USB»",
              L"Umgestellt auf „USB bevorzugen“",
              L"Mode « USB en priorité » activé",
              L"Đã chuyển sang “Ưu tiên USB”",
              L"เปลี่ยนเป็น “ใช้ USB เป็นหลัก” แล้ว",
              L"Переключено на «Приоритет USB»"}},
            {ui_text::switched_to_wifi_only,
             {L"已切換為僅 Wi-Fi",
              L"已切换为仅 Wi-Fi",
              L"Switched to Wi-Fi only",
              L"「Wi-Fi のみ」に切り替えました",
              L"‘Wi-Fi 전용’으로 전환했습니다",
              L"Se cambió a «Solo Wi-Fi»",
              L"Umgestellt auf „Nur Wi-Fi“",
              L"Mode « Wi-Fi uniquement » activé",
              L"Đã chuyển sang “Chỉ Wi-Fi”",
              L"เปลี่ยนเป็น “Wi-Fi เท่านั้น” แล้ว",
              L"Переключено на «Только Wi-Fi»"}},
            {ui_text::choose_receiver_again,
             {L"請在 iPhone 重新選擇「{}」。",
              L"请在 iPhone 上重新选择“{}”。",
              L"Choose \"{}\" again on the iPhone.",
              L"iPhone で「{}」を選び直してください。",
              L"iPhone에서 ‘{}’ 항목을 다시 선택하세요.",
              L"Vuelva a elegir «{}» en el iPhone.",
              L"Wählen Sie „{}“ auf dem iPhone erneut aus.",
              L"Choisissez de nouveau « {} » sur l’iPhone.",
              L"Hãy chọn lại “{}” trên iPhone.",
              L"เลือก “{}” อีกครั้งบน iPhone",
              L"Выберите «{}» на iPhone заново."}},
            {ui_text::usb_when_hotspot_on,
             {L"接上 USB 線並開啟「個人熱點」時，鏡像走 USB。",
              L"接上 USB 线并打开“个人热点”时，镜像走 USB。",
              L"Mirroring uses USB while the cable is plugged in and Personal Hotspot is on.",
              L"ケーブルを接続し、「インターネット共有」をオンにしている間は、USB でミラーリングします。",
              L"케이블이 연결되어 있고 ‘개인용 핫스팟’이 켜져 있으면 USB로 미러링합니다.",
              L"La duplicación usa USB mientras el cable esté conectado y «Punto de acceso personal» esté activa"
              L"do.",
              L"Die Spiegelung läuft über USB, solange das Kabel angeschlossen und der persönliche Hotspot aktiv"
              L"iert ist.",
              L"La recopie de l’écran passe par l’USB tant que le câble est branché et que « Partage de connexio"
              L"n » est activé.",
              L"Việc phản chiếu dùng USB khi cáp đang cắm và Điểm truy cập cá nhân đang bật.",
              L"การสะท้อนภาพหน้าจอจะใช้ USB ขณะที่เสียบสายและเปิด “ฮอตสปอตส่วนบุคคล” อยู่",
              L"Дублирование экрана идет по USB, пока кабель подключен и включен «Режим модема»."}},
            {ui_text::usb_charging_only,
             {L"USB 線僅供充電；鏡像一律走 Wi-Fi。",
              L"USB 线仅用于充电；镜像一律走 Wi-Fi。",
              L"The USB cable only charges; mirroring always uses Wi-Fi.",
              L"USB ケーブルは充電専用です。ミラーリングには常に Wi-Fi を使用します。",
              L"USB 케이블은 충전만 하며, 미러링에는 항상 Wi-Fi를 사용합니다.",
              L"El cable USB solo carga; la duplicación siempre usa Wi-Fi.",
              L"Das USB-Kabel lädt nur; die Spiegelung läuft immer über Wi-Fi.",
              L"Le câble USB sert uniquement à la charge ; la recopie de l’écran passe toujours par le Wi-Fi.",
              L"Cáp USB chỉ dùng để sạc; việc phản chiếu luôn dùng Wi-Fi.",
              L"สาย USB ใช้ชาร์จอย่างเดียว การสะท้อนภาพหน้าจอจะใช้ Wi-Fi เสมอ",
              L"USB-кабель только заряжает iPhone; дублирование экрана всегда идет по Wi-Fi."}},
            // notifications: mode
            {ui_text::mode_quality_text,
             {L"畫面最細緻：完整解析度，並多保留一點緩衝，不跳過任何畫面。",
              L"画面最细腻：完整分辨率，并多保留一点缓冲，不跳过任何画面。",
              L"The sharpest picture: full resolution and a little more buffering, no frame skipped.",
              L"最も鮮明な映像: フル解像度で、バッファリングをやや多めに取り、フレームを一切スキップしません。",
              L"가장 선명한 화면: 전체 해상도로 받고 버퍼링을 조금 더 두며, 프레임을 건너뛰지 않습니다.",
              L"La imagen más nítida: resolución completa y un poco más de búfer, sin omitir ningún fotograma.",
              L"Das schärfste Bild: volle Auflösung und etwas mehr Pufferung, kein Einzelbild wird übersprungen.",
              L"L’image la plus nette : pleine résolution, un peu plus de mise en tampon, aucune image sautée.",
              L"Hình ảnh sắc nét nhất: độ phân giải đầy đủ, bộ đệm nhiều hơn một chút, không bỏ khung hình nào.",
              L"ภาพคมชัดที่สุด: ความละเอียดเต็ม บัฟเฟอร์เพิ่มขึ้นเล็กน้อย และไม่ข้ามเฟรมใดเลย",
              L"Самое четкое изображение: полное разрешение, чуть больший буфер, ни один кадр не пропускается."}},
            {ui_text::mode_balanced_text,
             {L"依螢幕、顯示卡與網路狀況自動調整。",
              L"根据屏幕、显卡和网络状况自动调整。",
              L"Adapts to the monitor, the GPU and the network.",
              L"ディスプレイ、GPU、ネットワークの状況に合わせて自動的に調整します。",
              L"모니터, GPU, 네트워크 상태에 맞춰 자동으로 조정합니다.",
              L"Se adapta al monitor, a la GPU y a la red.",
              L"Passt sich an Monitor, GPU und Netzwerk an.",
              L"S’adapte à l’écran, au GPU et au réseau.",
              L"Tự điều chỉnh theo màn hình, GPU và mạng.",
              L"ปรับตามจอภาพ การ์ดจอ และเครือข่ายโดยอัตโนมัติ",
              L"Подстраивается под монитор, видеокарту и сеть."}},
            {ui_text::mode_performance_text,
             {L"延遲最低、最省資源：較輕的串流，緩衝減到最少。",
              L"延迟最低、最省资源：更轻量的视频流，缓冲降到最低。",
              L"The lowest delay and load: a lighter stream and minimal buffering.",
              L"遅延と負荷を最小に: 軽めのストリームを使い、バッファリングを最小限に抑えます。",
              L"가장 짧은 지연과 가장 적은 부하: 더 가벼운 스트림을 받고 버퍼링을 최소화합니다.",
              L"El menor retardo y la menor carga: una transmisión más ligera y un búfer mínimo.",
              L"Geringste Verzögerung und Last: ein schlankerer Stream und minimale Pufferung.",
              L"Latence et charge minimales : un flux allégé et une mise en tampon réduite au minimum.",
              L"Độ trễ và mức tải thấp nhất: luồng nhẹ hơn, bộ đệm tối thiểu.",
              L"ความหน่วงต่ำสุดและใช้ทรัพยากรน้อยสุด: สตรีมที่เบาลงและบัฟเฟอร์น้อยที่สุด",
              L"Наименьшая задержка и нагрузка: облегченный поток и минимальный буфер."}},
            {ui_text::mode_next_connection,
             {L"串流解析度的變更會在下次連線時生效。",
              L"视频流分辨率的更改将在下次连接时生效。",
              L"The stream resolution changes from the next connection.",
              L"ストリームの解像度は、次回の接続から変わります。",
              L"스트림 해상도는 다음 연결부터 바뀝니다.",
              L"La resolución de la transmisión cambia a partir de la próxima conexión.",
              L"Die Stream-Auflösung ändert sich ab der nächsten Verbindung.",
              L"La résolution du flux changera à la prochaine connexion.",
              L"Độ phân giải luồng sẽ thay đổi từ lần kết nối tiếp theo.",
              L"ความละเอียดของสตรีมจะเปลี่ยนเมื่อเชื่อมต่อครั้งถัดไป",
              L"Разрешение потока изменится при следующем подключении."}},
            // notifications: program
            {ui_text::ready_title,
             {L"已就緒",
              L"已就绪",
              L"Ready",
              L"準備完了",
              L"준비 완료",
              L"Listo",
              L"Bereit",
              L"Prêt",
              L"Sẵn sàng",
              L"พร้อมใช้งาน",
              L"Готов к работе"}},
            {ui_text::already_running_title,
             {L"已在執行中",
              L"已在运行",
              L"Already running",
              L"すでに実行中です",
              L"이미 실행 중입니다",
              L"Ya se está ejecutando",
              L"Wird bereits ausgeführt",
              L"Déjà en cours d’exécution",
              L"Chương trình đã chạy sẵn",
              L"โปรแกรมทำงานอยู่แล้ว",
              L"Уже запущен"}},
            {ui_text::select_on_iphone,
             {L"在 iPhone 控制中心的「螢幕鏡像輸出」選擇「{}」。",
              L"在 iPhone 控制中心的“屏幕镜像”中选择“{}”。",
              L"On the iPhone, open Control Center > Screen Mirroring and choose \"{}\".",
              L"iPhone でコントロールセンターを開き、「画面ミラーリング」で「{}」を選んでください。",
              L"iPhone에서 제어 센터 > 화면 미러링을 열고 ‘{}’ 항목을 선택하세요.",
              L"En el iPhone, abra el Centro de control > Duplicar pantalla y elija «{}».",
              L"Öffnen Sie auf dem iPhone Kontrollzentrum > Bildschirmsynchronisierung und wählen Sie „{}“.",
              L"Sur l’iPhone, ouvrez le centre de contrôle > Recopie de l’écran et choisissez « {} ».",
              L"Trên iPhone, mở Trung tâm điều khiển > Phản chiếu màn hình rồi chọn “{}”.",
              L"บน iPhone ให้เปิด ศูนย์ควบคุม > การสะท้อนภาพหน้าจอ แล้วเลือก “{}”",
              L"На iPhone откройте Пункт управления, нажмите «Повтор экрана» и выберите «{}»."}},
            {ui_text::still_running_title,
             {L"仍在背景執行",
              L"仍在后台运行",
              L"Still running in the background",
              L"バックグラウンドで実行中です",
              L"백그라운드에서 계속 실행 중입니다",
              L"Sigue ejecutándose en segundo plano",
              L"Läuft im Hintergrund weiter",
              L"Toujours en cours d’exécution en arrière-plan",
              L"Vẫn đang chạy trong nền",
              L"ยังทำงานอยู่เบื้องหลัง",
              L"MirrorDock продолжает работать в фоне"}},
            {ui_text::still_running_text,
             {L"可從通知區域的圖示結束程式。",
              L"可从通知区域的图标退出程序。",
              L"Use the notification-area icon to exit.",
              L"終了するには、通知領域のアイコンを使用してください。",
              L"종료하려면 알림 영역의 아이콘을 사용하세요.",
              L"Para salir, use el icono del área de notificación.",
              L"Zum Beenden nutzen Sie das Symbol im Infobereich.",
              L"Pour quitter, utilisez l’icône de la zone de notification.",
              L"Dùng biểu tượng ở khu vực thông báo để thoát.",
              L"หากต้องการออก ให้ใช้ไอคอนในพื้นที่แจ้งให้ทราบ",
              L"Выйти из программы можно через значок в области уведомлений."}},
            {ui_text::start_failed_title,
             {L"無法啟動",
              L"无法启动",
              L"Could not start",
              L"起動できませんでした",
              L"시작할 수 없습니다",
              L"No se pudo iniciar",
              L"Start fehlgeschlagen",
              L"Démarrage impossible",
              L"Không khởi động được",
              L"เริ่มโปรแกรมไม่ได้",
              L"Не удалось запустить программу"}},
            {ui_text::start_failed_text,
             {L"{}\n\n詳細資訊請見記錄檔：\n{}",
              L"{}\n\n详细信息请见日志：\n{}",
              L"{}\n\nDetails are in the log file:\n{}",
              L"{}\n\n詳細はログファイルを確認してください:\n{}",
              L"{}\n\n자세한 내용은 다음 로그 파일을 확인하세요.\n{}",
              L"{}\n\nLos detalles están en el archivo de registro:\n{}",
              L"{}\n\nDetails stehen in der Protokolldatei:\n{}",
              L"{}\n\nLes détails figurent dans le fichier journal :\n{}",
              L"{}\n\nXem chi tiết trong tệp nhật ký:\n{}",
              L"{}\n\nดูรายละเอียดได้ในไฟล์บันทึก:\n{}",
              L"{}\n\nПодробности — в файле журнала:\n{}"}},
            {ui_text::autostart_failed,
             {L"無法變更「開機時啟動」，詳見記錄檔。",
              L"无法更改“开机时启动”，详见日志。",
              L"Could not change \"Start with Windows\"; see the log file.",
              L"「Windows 起動時に実行」の設定を変更できませんでした。ログファイルを確認してください。",
              L"‘Windows 시작 시 실행’ 설정을 변경하지 못했습니다. 로그 파일을 확인하세요.",
              L"No se pudo cambiar «Iniciar con Windows»; consulte el archivo de registro.",
              L"„Mit Windows starten“ konnte nicht geändert werden; siehe Protokolldatei.",
              L"Impossible de modifier « Lancer au démarrage de Windows » ; consultez le fichier journal.",
              L"Không thay đổi được “Khởi động cùng Windows”; xem tệp nhật ký.",
              L"เปลี่ยนการตั้งค่า “เริ่มทำงานพร้อม Windows” ไม่สำเร็จ โปรดดูไฟล์บันทึก",
              L"Не удалось изменить параметр «Запускать вместе с Windows». Подробности — в файле журнала."}},
            {ui_text::pairing_pin_title,
             {L"配對 PIN 碼",
              L"配对 PIN 码",
              L"Pairing PIN",
              L"ペアリング用 PIN",
              L"페어링 PIN",
              L"PIN de emparejamiento",
              L"Kopplungs-PIN",
              L"Code PIN de jumelage",
              L"Mã PIN ghép đôi",
              L"PIN สำหรับจับคู่",
              L"PIN-код сопряжения"}},
            {ui_text::about_text,
             {L"iPhone／iPad 螢幕鏡像與音訊接收器（Windows 原生）\n\n版本 {}（協定核心：UxPlay {}）\n授權：GPL-3.0；媒體引擎為 MIT\n{}",
              L"iPhone 和 iPad 屏幕镜像与音频接收器（Windows 原生）\n\n版本 {}（协议核心：UxPlay {}）\n许可证：GPL-3.0；媒体引擎为 MIT\n{}",
              L"Screen mirroring and audio receiver for iPhone and iPad (Windows-native)\n\nVersion {} (protocol"
              L" core: UxPlay {})\nLicence: GPL-3.0; media engine MIT\n{}",
              L"iPhone / iPad 用の画面ミラーリング・オーディオレシーバー（Windows ネイティブ）\n\nバージョン {}（プロトコルコア: UxPlay {}）\nライセンス: GPL-3"
              L".0（メディアエンジンは MIT）\n{}",
              L"iPhone·iPad용 화면 미러링 및 오디오 수신기(Windows 네이티브)\n\n버전 {}(프로토콜 코어: UxPlay {})\n라이선스: GPL-3.0, 미디어 엔진은"
              L" MIT\n{}",
              L"Receptor de duplicación de pantalla y de audio para iPhone y iPad (nativo de Windows)\n\nVersión"
              L" {} (núcleo de protocolo: UxPlay {})\nLicencia: GPL-3.0; motor multimedia: MIT\n{}",
              L"Empfänger für Bildschirmspiegelung und Audio von iPhone und iPad (nativ für Windows)\n\nVersion "
              L"{} (Protokollkern: UxPlay {})\nLizenz: GPL-3.0; Medien-Engine: MIT\n{}",
              L"Récepteur de recopie d’écran et audio pour iPhone et iPad (natif Windows)\n\nVersion {} (cœur de"
              L" protocole : UxPlay {})\nLicence : GPL-3.0 ; moteur multimédia : MIT\n{}",
              L"Bộ thu phản chiếu màn hình và âm thanh từ iPhone và iPad (viết riêng cho Windows)\n\nPhiên bản {"
              L"} (lõi giao thức: UxPlay {})\nGiấy phép: GPL-3.0; bộ máy xử lý đa phương tiện: MIT\n{}",
              L"ตัวรับการสะท้อนภาพหน้าจอและเสียงสำหรับ iPhone และ iPad (เนทีฟสำหรับ Windows)\n\nเวอร์ชัน {} (แกน"
              L"โปรโตคอล: UxPlay {})\nสัญญาอนุญาต: GPL-3.0 ส่วนเอนจินสื่อเป็น MIT\n{}",
              L"Дублирование экрана и прием звука с iPhone и iPad (нативное приложение для Windows)\n\nВерсия {}"
              L" (ядро протокола: UxPlay {})\nЛицензия: GPL-3.0; медиадвижок — MIT\n{}"}},
            // cropping
            {ui_text::crop_off,
             {L"不裁切",
              L"不裁剪",
              L"No cropping",
              L"トリミングなし",
              L"자르지 않음",
              L"Sin recorte",
              L"Kein Zuschnitt",
              L"Aucun rognage",
              L"Không cắt",
              L"ไม่ครอบตัด",
              L"Без обрезки"}},
            {ui_text::crop_automatic,
             {L"自動裁切黑邊",
              L"自动裁剪黑边",
              L"Crop black bars automatically",
              L"黒帯を自動でトリミング",
              L"검은 여백 자동으로 자르기",
              L"Recortar barras negras automáticamente",
              L"Schwarze Balken automatisch abschneiden",
              L"Rogner automatiquement les bandes noires",
              L"Tự động cắt viền đen",
              L"ครอบตัดขอบดำอัตโนมัติ",
              L"Автоматически обрезать черные полосы"}},
            {ui_text::crop_select,
             {L"框選裁切範圍…",
              L"框选裁剪范围…",
              L"Select the area to show…",
              L"表示する範囲を選択…",
              L"표시할 영역 선택…",
              L"Seleccionar el área visible…",
              L"Bildausschnitt auswählen…",
              L"Sélectionner la zone à afficher…",
              L"Chọn vùng hiển thị…",
              L"เลือกพื้นที่ที่จะแสดง…",
              L"Выделить область для показа…"}},
            {ui_text::crop_select_hint,
             {L"拖曳框出要保留的範圍 · Esc 取消",
              L"拖动框出要保留的范围 · Esc 取消",
              L"Drag to frame the area to keep · Esc cancels",
              L"ドラッグして残す範囲を囲む · Esc でキャンセル",
              L"드래그하여 남길 영역 지정 · Esc로 취소",
              L"Arrastre para enmarcar el área que desea conservar · Esc cancela",
              L"Rahmen um den gewünschten Bereich ziehen · Esc bricht ab",
              L"Tracez un cadre autour de la zone à conserver · Esc pour annuler",
              L"Kéo để khoanh vùng muốn giữ lại · Esc để hủy",
              L"ลากกรอบรอบส่วนที่ต้องการเก็บไว้ · กด Esc เพื่อยกเลิก",
              L"Обведите рамкой нужную область · Esc — отмена"}},
            {ui_text::cropped,
             {L"{} · 已裁切",
              L"{} · 已裁剪",
              L"{} · cropped",
              L"{} · トリミング済み",
              L"{} · 잘라냄",
              L"{} · con recorte",
              L"{} · zugeschnitten",
              L"{} · rogné",
              L"{} · đã cắt",
              L"{} · ครอบตัดแล้ว",
              L"{} · обрезано"}},
            // remote control of the phone
            {ui_text::control_phone,
             {L"用滑鼠和鍵盤操作 iPhone",
              L"用鼠标和键盘操作 iPhone",
              L"Control the iPhone with mouse and keyboard",
              L"マウスとキーボードで iPhone を操作",
              L"마우스와 키보드로 iPhone 제어",
              L"Controlar el iPhone con mouse y teclado",
              L"iPhone mit Maus und Tastatur steuern",
              L"Contrôler l’iPhone à la souris et au clavier",
              L"Điều khiển iPhone bằng chuột và bàn phím",
              L"ควบคุม iPhone ด้วยเมาส์และคีย์บอร์ด",
              L"Управление iPhone мышью и клавиатурой"}},
            {ui_text::control_connecting,
             {L"正在連接 iPhone…",
              L"正在连接 iPhone…",
              L"connecting to the iPhone…",
              L"iPhone に接続中…",
              L"iPhone에 연결 중…",
              L"conectando con el iPhone…",
              L"wird mit dem iPhone verbunden…",
              L"connexion à l’iPhone…",
              L"đang kết nối với iPhone…",
              L"กำลังเชื่อมต่อกับ iPhone…",
              L"подключение к iPhone…"}},
            {ui_text::control_ready_title,
             {L"現在可以操作 iPhone 了",
              L"现在可以操作 iPhone 了",
              L"You can control the iPhone now",
              L"iPhone を操作できるようになりました",
              L"이제 iPhone을 제어할 수 있습니다",
              L"Ya puede controlar el iPhone",
              L"Sie können das iPhone jetzt steuern",
              L"Vous pouvez maintenant contrôler l’iPhone",
              L"Bây giờ bạn có thể điều khiển iPhone",
              L"ควบคุม iPhone ได้แล้ว",
              L"Теперь можно управлять iPhone"}},
            {ui_text::control_ready_text,
             {L"直接在畫面上點擊、拖曳和打字。按右鍵開啟選單，按 F8 關閉操作。",
              L"直接在画面上点击、拖动和打字。右键打开菜单，按 F8 关闭操作。",
              L"Click, drag and type on the picture. Right-click opens the menu, F8 switches control off.",
              L"映像の上でクリック、ドラッグ、キーボード入力ができます。右クリックでメニューが開き、F8 で操作をオフにできます。",
              L"영상 위에서 클릭하고, 드래그하고, 키보드로 입력하세요. 오른쪽 클릭하면 메뉴가 열리고, F8을 누르면 제어가 꺼집니다.",
              L"Haga clic, arrastre y escriba sobre la imagen. El botón derecho abre el menú y F8 desactiva el c"
              L"ontrol.",
              L"Klicken, ziehen und schreiben Sie direkt im Bild. Ein Rechtsklick öffnet das Menü, F8 schaltet d"
              L"ie Steuerung aus.",
              L"Cliquez, faites glisser et tapez au clavier, directement sur l’image. Le clic droit ouvre le men"
              L"u, F8 désactive le contrôle.",
              L"Hãy bấm, kéo và gõ phím ngay trên hình ảnh. Bấm chuột phải để mở menu, nhấn F8 để tắt điều khiển"
              L".",
              L"คลิก ลาก และพิมพ์บนภาพได้เลย คลิกขวาเพื่อเปิดเมนู กด F8 เพื่อปิดการควบคุม",
              L"Щелкайте, перетаскивайте и печатайте прямо на изображении. Правая кнопка мыши открывает меню, F8"
              L" выключает управление."}},
            {ui_text::control_preparing_title,
             {L"正在準備 iPhone",
              L"正在准备 iPhone",
              L"Preparing the iPhone",
              L"iPhone を準備しています",
              L"iPhone을 준비하고 있습니다",
              L"Preparando el iPhone",
              L"iPhone wird vorbereitet",
              L"Préparation de l’iPhone",
              L"Đang chuẩn bị iPhone",
              L"กำลังเตรียม iPhone",
              L"Подготовка iPhone"}},
            {ui_text::control_preparing_text,
             {L"第一次操作（以及 iOS 更新後的第一次）需要為 iPhone 安裝 Apple 的開發者元件，大約一分鐘，需要網際網路連線。",
              L"首次操作（以及 iOS 更新后的第一次）需要为 iPhone 安装 Apple 的开发者组件，大约一分钟，需要互联网连接。",
              L"The first use (and the first after an iOS update) installs Apple's developer components on the i"
              L"Phone. This takes about a minute and needs an internet connection.",
              L"初めて使うとき（および iOS をアップデートしたあとの初回）は、Apple の開発者向けコンポーネントを iPhone にインストールします。約 1 分かかり、インターネット接続が必要です。",
              L"처음 사용할 때(그리고 iOS 업데이트 후 처음 사용할 때)는 iPhone에 Apple의 개발자용 구성 요소를 설치합니다. 1분 정도 걸리며 인터넷 연결이 필요합니다.",
              L"La primera vez (y la primera tras una actualización de iOS) se instalan en el iPhone los compone"
              L"ntes para desarrolladores de Apple. Tarda cerca de un minuto y requiere conexión a Internet.",
              L"Bei der ersten Nutzung (und der ersten nach einem iOS-Update) werden Apples Entwicklerkomponente"
              L"n auf dem iPhone installiert. Das dauert etwa eine Minute und erfordert eine Internetverbindung.",
              L"La première utilisation (et la première après une mise à jour d’iOS) installe sur l’iPhone les c"
              L"omposants pour développeurs d’Apple. Il faut environ une minute et une connexion Internet.",
              L"Ở lần dùng đầu tiên (và lần đầu tiên sau khi cập nhật iOS), các thành phần dành cho nhà phát tri"
              L"ển của Apple sẽ được cài lên iPhone. Việc này mất khoảng một phút và cần có kết nối Internet.",
              L"ในการใช้งานครั้งแรก (และครั้งแรกหลังอัปเดต iOS) โปรแกรมจะติดตั้งส่วนประกอบสำหรับนักพัฒนาของ Appl"
              L"e ลงใน iPhone ซึ่งใช้เวลาประมาณหนึ่งนาทีและต้องเชื่อมต่ออินเทอร์เน็ต",
              L"При первом использовании (и первом после обновления iOS) на iPhone устанавливаются компоненты Ap"
              L"ple для разработчиков. Это занимает около минуты и требует подключения к интернету."}},
            {ui_text::control_failed_title,
             {L"無法操作 iPhone",
              L"无法操作 iPhone",
              L"The iPhone cannot be controlled",
              L"iPhone を操作できません",
              L"iPhone을 제어할 수 없습니다",
              L"No se puede controlar el iPhone",
              L"Das iPhone lässt sich nicht steuern",
              L"Impossible de contrôler l’iPhone",
              L"Không điều khiển được iPhone",
              L"ควบคุม iPhone ไม่ได้",
              L"Не удается управлять iPhone"}},
            {ui_text::control_needs_cable,
             {L"請用 USB 線連接 iPhone，解鎖後點一下「信任」。",
              L"请用 USB 数据线连接 iPhone，解锁后轻点“信任”。",
              L"Connect the iPhone with the USB cable, unlock it and tap \"Trust\".",
              L"iPhone を USB ケーブルで接続し、ロックを解除して「信頼」をタップしてください。",
              L"iPhone을 USB 케이블로 연결하고 잠금을 해제한 다음 ‘신뢰’를 탭하세요.",
              L"Conecte el iPhone con el cable USB, desbloquéelo y toque «Confiar».",
              L"Schließen Sie das iPhone mit dem USB-Kabel an, entsperren Sie es und tippen Sie auf „Vertrauen“.",
              L"Branchez l’iPhone avec le câble USB, déverrouillez-le et touchez « Se fier ».",
              L"Hãy kết nối iPhone bằng cáp USB, mở khóa rồi chạm vào “Tin cậy”.",
              L"เชื่อมต่อ iPhone ด้วยสาย USB ปลดล็อกเครื่อง แล้วแตะ “เชื่อถือ”",
              L"Подключите iPhone USB-кабелем, разблокируйте его и нажмите «Доверять»."}},
            {ui_text::control_needs_developer_mode,
             {L"請在 iPhone 開啟開發者模式：設定 > 隱私權與安全性 > 開發者模式。",
              L"请在 iPhone 上打开开发者模式：设置 > 隐私与安全性 > 开发者模式。",
              L"Turn on Developer Mode on the iPhone: Settings > Privacy & Security > Developer Mode.",
              L"iPhone でデベロッパモードをオンにしてください（設定 > プライバシーとセキュリティ > デベロッパモード）。",
              L"iPhone의 설정 > 개인정보 보호 및 보안 > 개발자 모드에서 개발자 모드를 켜세요.",
              L"Active el modo de desarrollador en el iPhone: Ajustes (o Configuración) > Privacidad y seguridad"
              L" > Modo de desarrollador.",
              L"Aktivieren Sie auf dem iPhone den Entwicklermodus: Einstellungen > Datenschutz & Sicherheit > En"
              L"twicklermodus.",
              L"Activez le mode Développeur sur l’iPhone : Réglages > Confidentialité et sécurité > Mode Dévelop"
              L"peur.",
              L"Hãy bật Chế độ nhà phát triển trên iPhone: Cài đặt > Quyền riêng tư & Bảo mật > Chế độ nhà phát "
              L"triển.",
              L"เปิดโหมดนักพัฒนาบน iPhone: การตั้งค่า > ความเป็นส่วนตัวและความปลอดภัย > โหมดนักพัฒนา",
              L"Включите на iPhone режим разработчика: «Настройки» > «Конфиденциальность и безопасность» > «Режи"
              L"м разработчика»."}},
            {ui_text::control_image_failed,
             {L"無法把開發者磁碟映像檔載入 iPhone。請確認這台電腦可以上網，然後再試一次。",
              L"无法把开发者磁盘映像加载到 iPhone。请确认这台电脑可以上网，然后重试。",
              L"The Developer Disk Image could not be loaded onto the iPhone. Check that this PC is online, then"
              L" try again.",
              L"開発者向けディスクイメージを iPhone にインストールできませんでした。この PC がインターネットに接続されているか確認して、もう一度お試しください。",
              L"개발자 디스크 이미지를 iPhone에 설치하지 못했습니다. 이 PC가 인터넷에 연결되어 있는지 확인한 다음 다시 시도하세요.",
              L"No se pudo cargar en el iPhone la imagen de disco de desarrollador. Compruebe que este equipo te"
              L"nga conexión a Internet y vuelva a intentarlo.",
              L"Das Developer Disk Image konnte nicht auf das iPhone geladen werden. Prüfen Sie, ob dieser PC on"
              L"line ist, und versuchen Sie es dann erneut.",
              L"Impossible de charger l’image disque développeur sur l’iPhone. Vérifiez que ce PC est connecté à"
              L" Internet, puis réessayez.",
              L"Không nạp được Developer Disk Image vào iPhone. Hãy kiểm tra kết nối Internet của máy tính này r"
              L"ồi thử lại.",
              L"โหลด Developer Disk Image (ดิสก์อิมเมจสำหรับนักพัฒนา) ลงใน iPhone ไม่สำเร็จ โปรดตรวจสอบว่าคอมพิว"
              L"เตอร์เครื่องนี้เชื่อมต่ออินเทอร์เน็ตอยู่ แล้วลองอีกครั้ง",
              L"Не удалось установить на iPhone образ диска разработчика. Проверьте, что компьютер подключен к и"
              L"нтернету, и повторите попытку."}},
            {ui_text::control_camera_in_use,
             {L"iPhone 的相機或麥克風正在使用中。請關閉使用它的 App，然後再試一次。",
              L"iPhone 的相机或麦克风正在使用中。请关闭使用它的 App，然后重试。",
              L"The iPhone's camera or microphone is in use. Close the app that uses it, then try again.",
              L"iPhone のカメラまたはマイクが使用中です。使用しているアプリを閉じてから、もう一度お試しください。",
              L"iPhone의 카메라 또는 마이크가 사용 중입니다. 사용 중인 앱을 닫은 다음 다시 시도하세요.",
              L"La cámara o el micrófono del iPhone están en uso. Cierre la app que los usa y vuelva a intentarl"
              L"o.",
              L"Die Kamera oder das Mikrofon des iPhone wird gerade verwendet. Schließen Sie die App, die darauf"
              L" zugreift, und versuchen Sie es dann erneut.",
              L"L’appareil photo ou le micro de l’iPhone est en cours d’utilisation. Fermez l’app qui s’en sert,"
              L" puis réessayez.",
              L"Camera hoặc micrô của iPhone đang được sử dụng. Hãy đóng ứng dụng đang dùng camera hoặc micrô rồ"
              L"i thử lại.",
              L"กล้องหรือไมโครโฟนของ iPhone กำลังถูกใช้งานอยู่ โปรดปิดแอปที่ใช้กล้องหรือไมโครโฟน แล้วลองอีกครั้ง",
              L"Камера или микрофон iPhone сейчас заняты. Закройте приложение, которое их использует, и повторит"
              L"е попытку."}},
            {ui_text::control_needs_ios,
             {L"操作 iPhone 需要 iOS 27 或以上版本。",
              L"操作 iPhone 需要 iOS 27 或更高版本。",
              L"Controlling the iPhone needs iOS 27 or later.",
              L"iPhone の操作には iOS 27 以降が必要です。",
              L"iPhone을 제어하려면 iOS 27 이상이 필요합니다.",
              L"Para controlar el iPhone se necesita iOS 27 o posterior.",
              L"Für die Steuerung des iPhone ist iOS 27 oder neuer erforderlich.",
              L"Le contrôle de l’iPhone nécessite iOS 27 ou version ultérieure.",
              L"Để điều khiển iPhone, cần có iOS 27 trở lên.",
              L"การควบคุม iPhone ต้องใช้ iOS 27 ขึ้นไป",
              L"Для управления iPhone нужна iOS 27 или новее."}},
            {ui_text::control_failed_text,
             {L"詳細資訊請見記錄檔。",
              L"详细信息请见日志。",
              L"Details are in the log file.",
              L"詳細はログファイルを確認してください。",
              L"자세한 내용은 로그 파일을 확인하세요.",
              L"Los detalles están en el archivo de registro.",
              L"Details stehen in der Protokolldatei.",
              L"Les détails figurent dans le fichier journal.",
              L"Xem chi tiết trong tệp nhật ký.",
              L"ดูรายละเอียดได้ในไฟล์บันทึก",
              L"Подробности — в файле журнала."}},
            {ui_text::controlled,
             {L"{} · 可操作",
              L"{} · 可操作",
              L"{} · control",
              L"{} · 操作中",
              L"{} · 제어 중",
              L"{} · control activo",
              L"{} · Steuerung",
              L"{} · contrôle actif",
              L"{} · đang điều khiển",
              L"{} · ควบคุมได้",
              L"{} · управление"}},
        }};

        consteval bool text_table_is_complete()
        {
            for (std::size_t index = 0; index < text_table.size(); ++index)
            {
                text_row const &row = text_table[index];
                if (static_cast<std::size_t>(row.id) != index)
                {
                    return false;
                }
                for (std::wstring_view const text : row.text)
                {
                    if (text.empty())
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        static_assert(text_table_is_complete(),
                      "every ui_text needs one row, in enum order, with a text in every language");

        // ui_format fills the "{}" of a text in order: every language must have
        // as many of them as the English text
        consteval std::size_t placeholder_count(std::wstring_view text)
        {
            std::size_t count = 0;
            for (std::size_t index = 0; index + 1 < text.size(); ++index)
            {
                if (text[index] == L'{' && text[index + 1] == L'}')
                {
                    ++count;
                    ++index;
                }
            }
            return count;
        }

        consteval bool placeholders_agree()
        {
            for (text_row const &row : text_table)
            {
                for (std::wstring_view const text : row.text)
                {
                    if (placeholder_count(text) != placeholder_count(row.text[english_index]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        static_assert(placeholders_agree(), "a text has another number of {} placeholders than its English version");

        // set once by native_media_start before the engine threads exist
        std::atomic<ui_language> active_language{ui_language::english};

        bool equals_ascii_lowercase(std::wstring_view subtag, std::wstring_view lowercase)
        {
            if (subtag.size() != lowercase.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < subtag.size(); ++index)
            {
                wchar_t const character = subtag[index];
                wchar_t const lowered = character >= L'A' && character <= L'Z'
                                            ? static_cast<wchar_t>(character - L'A' + L'a')
                                            : character;
                if (lowered != lowercase[index])
                {
                    return false;
                }
            }
            return true;
        }
    }

    ui_language ui_language_for(std::wstring_view windows_language)
    {
        // BCP-47: language[-script][-region]...; '_' accepted as a separator too
        std::vector<std::wstring_view> subtags;
        std::size_t start = 0;
        while (start <= windows_language.size())
        {
            std::size_t const end = windows_language.find_first_of(L"-_", start);
            std::size_t const stop = end == std::wstring_view::npos ? windows_language.size() : end;
            subtags.push_back(windows_language.substr(start, stop - start));
            if (end == std::wstring_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        if (subtags.empty())
        {
            return ui_language::english;
        }
        // the primary subtag decides, except for Chinese, which has two scripts
        constexpr std::array<std::pair<std::wstring_view, ui_language>, 8> by_primary_subtag{{
            {L"ja", ui_language::japanese},
            {L"ko", ui_language::korean},
            {L"es", ui_language::spanish},
            {L"de", ui_language::german},
            {L"fr", ui_language::french},
            {L"vi", ui_language::vietnamese},
            {L"th", ui_language::thai},
            {L"ru", ui_language::russian},
        }};
        for (auto const &[subtag, language] : by_primary_subtag)
        {
            if (equals_ascii_lowercase(subtags.front(), subtag))
            {
                return language;
            }
        }
        if (!equals_ascii_lowercase(subtags.front(), L"zh"))
        {
            return ui_language::english;
        }
        // the script subtag decides; otherwise Taiwan, Hong Kong and Macao write Traditional Chinese
        for (std::wstring_view const subtag : std::span{subtags}.subspan(1))
        {
            if (equals_ascii_lowercase(subtag, L"hant"))
            {
                return ui_language::traditional_chinese;
            }
            if (equals_ascii_lowercase(subtag, L"hans"))
            {
                return ui_language::simplified_chinese;
            }
        }
        for (std::wstring_view const subtag : std::span{subtags}.subspan(1))
        {
            if (equals_ascii_lowercase(subtag, L"tw") || equals_ascii_lowercase(subtag, L"hk") ||
                equals_ascii_lowercase(subtag, L"mo"))
            {
                return ui_language::traditional_chinese;
            }
        }
        return ui_language::simplified_chinese;
    }

    outcome<ui_language_detection> detect_ui_language()
    {
        ULONG preferred_count = 0;
        ULONG buffer_length = 0;
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &preferred_count, nullptr, &buffer_length))
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages (size)", GetLastError()));
        }
        std::wstring buffer(buffer_length, L'\0');
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &preferred_count, buffer.data(), &buffer_length))
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages", GetLastError()));
        }
        // a multi-string: the first (most preferred) name ends at the first NUL
        std::wstring first{buffer.c_str()};
        if (preferred_count == 0 || first.empty())
        {
            return std::unexpected(make_win32_failure("GetUserPreferredUILanguages (empty list)", ERROR_NO_DATA));
        }
        ui_language_detection detection;
        detection.language = ui_language_for(first);
        detection.windows_language = std::move(first);
        return detection;
    }

    void set_ui_language(ui_language language)
    {
        active_language.store(language, std::memory_order_relaxed);
    }

    ui_language current_ui_language()
    {
        return active_language.load(std::memory_order_relaxed);
    }

    std::wstring_view ui_string(ui_text id)
    {
        auto const index = static_cast<std::size_t>(id);
        if (index >= text_table.size())
        {
            return L"?";
        }
        auto const language = static_cast<std::size_t>(current_ui_language());
        return text_table[index].text[language < language_count ? language : english_index];
    }

    std::wstring ui_format(ui_text id, std::span<std::wstring_view const> arguments)
    {
        std::wstring_view const pattern = ui_string(id);
        std::wstring result;
        result.reserve(pattern.size() + 64);
        std::size_t next_argument = 0;
        for (std::size_t index = 0; index < pattern.size(); ++index)
        {
            if (pattern[index] == L'{' && index + 1 < pattern.size() && pattern[index + 1] == L'}')
            {
                if (next_argument < arguments.size())
                {
                    result += arguments[next_argument];
                }
                ++next_argument;
                ++index;
            }
            else
            {
                result += pattern[index];
            }
        }
        return result;
    }

    std::wstring_view ui_locale_name()
    {
        auto const language = static_cast<std::size_t>(current_ui_language());
        return languages[language < language_count ? language : english_index].locale;
    }

    std::string_view ui_language_name(ui_language language)
    {
        auto const index = static_cast<std::size_t>(language);
        return languages[index < language_count ? index : english_index].code;
    }

    std::optional<ui_language> ui_language_from_code(std::string_view code)
    {
        auto const same = [](std::string_view first, std::string_view second)
        {
            return std::ranges::equal(first, second, [](char a, char b)
            {
                auto const lower = [](char c)
                {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                return lower(a) == lower(b);
            });
        };
        // the regional names people type for the two Chinese scripts
        if (same(code, "zh-TW"))
        {
            return ui_language::traditional_chinese;
        }
        if (same(code, "zh-CN"))
        {
            return ui_language::simplified_chinese;
        }
        for (std::size_t index = 0; index < language_count; ++index)
        {
            if (same(code, languages[index].code))
            {
                return static_cast<ui_language>(index);
            }
        }
        return std::nullopt;
    }
}
