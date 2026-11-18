Вариант 5. Сохранение и сравнение снимков учебного дерева реестра.
Среда: Windows, PowerShell, MinGW-w64 с C++20. Сборка и примеры: build.txt.
registry save student run file — снимок существующего дерева через RegEnumKeyExW, RegEnumValueW и RegQueryValueExW.
registry compare before after — сравнение снимков по путям, именам, типам и байтам значений.
registry.exe demo student new-directory — создаёт новый выходной каталог и проводит три опыта на 10, 50 и 100 изменений.
registry demo student run changes before after — создаёт дерево, сохраняет два снимка, проверяет тип и имя каждого ожидаемого изменения и удаляет созданное дерево.
Изменения ограничены HKCU\Software\OSiSP\Lab06\Student_<student>\Run_<run>. Выбирайте новый run и новые имена выходных файлов. Для эксперимента повторите demo три раза для 10, 50 и 100 изменений; для индивидуального запуска укажите последние три цифры студбилета.
