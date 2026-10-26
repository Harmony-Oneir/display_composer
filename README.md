# display_composer

`display_composer` — это низкоуровневый драйверный слой (HDI/HDF), который общается непосредственно с железом экрана и ядром Linux (через DRM/KMS драйвер и `display_buffer`).

Composer читает системный параметр `persist.oneir.composer.nonblock` и `oneir.composer.cpuwait`:
|Параметр|Значение по умолчанию|Что делает|
|--------|-----|---|
|`persist.oneir.composer.nonblock`|`0` — блокирующий коммит|1 — неблокирующий атомарный коммит (DRM_MODE_ATOMIC_NONBLOCK), то есть параллельная работа: RS готовит следующий кадр, пока предыдущий ждёт вывода на экран|
|`oneir.composer.cpuwait`|`0`|отладка: перед коммитом ждать acquire fence слоёв на CPU (до 100 мс)|

## Сборка
Для сборки выполните команду `./make.sh --reload  //oneir/display_composer:display_composer` внутри контейнера. Результат сборки располагается по пути `out/oneir/oneir/oneir_display_composer`:
| Библиотека | Описание |
|---|---|
| `libdisplay_composer_vdi_impl.z.so` | Точка входа VDI композитора: экспортирует `CreateComposerVdi` / `DestroyComposerVdi` и функции интерфейса `IDisplayComposerVdi` (экраны, режимы, слои, prepare, commit, VSync). Загружается HDI-сервисом композитора через `dlopen` |
| `libdisplay_composer_vendor.z.so` | DRM-логика композитора: выбор устройства через `OneirOpenDrmDevice()`, коннекторы, CRTC, plane'ы, режимы экрана, слои и их раскладка по plane'ам, атомарный коммит с fence'ами, VSync, hotplug через netlink и подсветка через `OneirBacklightPath()`. Использует буферную VDI из `oneir_display` для выделения и отображения буферов |
| `libdisplay_gfx.z.so` | Программная 2D-композиция (заливка, копирование, смешивание на CPU). Загружается композитором через `dlopen` и нужна для слоёв, которые нельзя ни положить на plane, ни отдать RS |



## Тестирование на стенде 
Для работы display_composer необходимо освободить экран, если он занят основной операционной системой на стенде:
```bash
sudo systemctl stop display-manager
sudo fuser -v /dev/dri/card1        # должно быть пусто
```

После запуска сервиса `ohos-composer-host` должны быть загружены следующие библиотеки:
```bash
P=$(pidof composer_host)
sudo grep -oE '/[^ ]+\.so' /proc/$P/maps | sort -u | grep -E "composer|vdi_impl|display_buffer|drm|gfx"
sudo ls -l /proc/$P/fd | grep dri

/system/lib64/chipset-sdk/libdrm.so
/vendor/lib64/libcomposer_buffer_cache.z.so
/vendor/lib64/libdisplay_buffer_stub_1.0.z.so
/vendor/lib64/libdisplay_buffer_stub_1.1.z.so
/vendor/lib64/libdisplay_buffer_stub_1.2.z.so
/vendor/lib64/libdisplay_buffer_vdi_impl.z.so
/vendor/lib64/libdisplay_buffer_vendor.z.so
/vendor/lib64/libdisplay_composer_driver_1.0.z.so
/vendor/lib64/libdisplay_composer_service_1.5.z.so
/vendor/lib64/libdisplay_composer_stub_1.0.z.so
/vendor/lib64/libdisplay_composer_stub_1.1.z.so
/vendor/lib64/libdisplay_composer_stub_1.2.z.so
/vendor/lib64/libdisplay_composer_stub_1.3.z.so
/vendor/lib64/libdisplay_composer_stub_1.4.z.so
/vendor/lib64/libdisplay_composer_stub_1.5.z.so
/vendor/lib64/libdisplay_composer_vdi_impl.z.so
/vendor/lib64/libdisplay_composer_vendor.z.so
```
Хост загрузил драйвер и службу Composer (`libdisplay_composer_driver_1.0`, `libdisplay_composer_service_1.5`), VDI (`libdisplay_composer_vdi_impl` + `libdisplay_composer_vendor`), буферный VDI (`libdisplay_buffer_vdi_impl` + `libdisplay_buffer_vendor`) и `libdrm`.

Для тестирования нужны следующие цели:
|Цель| Что проверяет |
|----|---------------|
|`unittest:composer_ut`|Интерфейс композитора: возможности экрана, режимы, питание, подсветка, VSync, создание слоёв и их свойства, prepare и commit|
| `moduletest:composer_mt` | Сценарии целиком: создаёт слои с тестовыми изображениями и выводит их на экран. Главная проверка цепочки до экрана |
| `benchmark:hdf_display_benchmark_test` | Скорость операций композитора |
| `unittest:composer_death_test` | Поведение при падении клиента |

Результаты сборки:
* `out/oneir/tests/moduletest/drivers_peripheral_display/drivers_peripheral_display_composer/composer_mt`
* `out/oneir/tests/unittest/drivers_peripheral_display/drivers_peripheral_display_composer/composer_ut`
* `out/oneir/tests/benchmark/drivers_peripheral_display/drivers_peripheral_display_composer/hdf_display_benchmark_test`
* `out/oneir/tests/unittest/drivers_peripheral_display/drivers_peripheral_display_composer/composer_death_test`
