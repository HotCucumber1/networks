### Как запустить приложение

1. Открыть терминал в папке проекта.
2. Скомпилировать программу:

```bash
g++ -std=c++20 smtp_client.cpp -o smtp_client -lssl -lcrypto
```

3. Запустить приложение:
```bash
./smtp_client \
  --server smtp.mail.ru --port 587 \
  --tls starttls --auth plain \
  --user 'user@mail.ru' \
  --password-env 'my-password \
  --from 'from@mail.ru' \
  --to 'to@example.com' \
  --subject 'Тест' \
  --body 'Привет!'
```

4. Также тут лежит скриптик `./send-to-real-client` чтобы просто его запустить, чтобы сразу все отправить