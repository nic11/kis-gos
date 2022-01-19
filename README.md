```sh
mkcd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .

# Запуск с кодированием (из полного формата ../log в минимизированный ../min)
./logmin --templates-path ../tpl --full-log-path ../log --min-log-path ../min -f

# Раскодирование
./logmin --templates-path ../tpl --full-log-path ../log --min-log-path ../min --decode -f
```

Tested on CMake 3.22.1, GCC 11.1.0
