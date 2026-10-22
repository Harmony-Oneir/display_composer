# display_composer

## Сборка
Для сборки выполните команду `./make.sh --reload  //oneir/display_composer:display_composer` внутри контейнера. Результат сборки располагается по пути `out/oneir/oneir/oneir_display_composer`:
| Библиотека | Описание |
|---|---|
| `libdisplay_composer_vdi_impl.z.so` | Точка входа VDI композитора: экспортирует `CreateComposerVdi` / `DestroyComposerVdi` и функции интерфейса `IDisplayComposerVdi` (экраны, режимы, слои, prepare, commit, VSync). Загружается HDI-сервисом композитора через `dlopen` |
| `libdisplay_composer_vendor.z.so` | DRM-логика композитора: выбор устройства через `OneirOpenDrmDevice()`, коннекторы, CRTC, plane'ы, режимы экрана, слои и их раскладка по plane'ам, атомарный коммит с fence'ами, VSync, hotplug через netlink и подсветка через `OneirBacklightPath()`. Использует буферную VDI из `oneir_display` для выделения и отображения буферов |
| `libdisplay_gfx.z.so` | Программная 2D-композиция (заливка, копирование, смешивание на CPU). Загружается композитором через `dlopen` и нужна для слоёв, которые нельзя ни положить на plane, ни отдать RS |
