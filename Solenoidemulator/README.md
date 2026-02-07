# Solenoidemulatorについて<br>
<img width="1280" height="670" alt="image" src="https://github.com/user-attachments/assets/06a9aac7-6331-4f61-9088-c659603a23d9" />



Solenoidemulaterは、PCと接続されたキーボードのタイピングに合わせてソレノイドを模倣した音色、バイブレーションによる感覚的なフィードバックを発生させるアプリケーションです。<br>

**<主な機能>**
- **Solenoidの稼働アニメーション**<br>
- **音量、音程、バイブレーション強度のカスタマイズ**<br>

**<基本構成>**
- **[Structure_1]**:自作キーボード[TheEndpoint](https://booth.pm/ja/items/5397024?srsltid=AfmBOopyI2rIjd3S4u9huHNX6b24YG9W6Mf1bb4Djlk5SMYoJj527o7B)(QMKFirmware・マイコン/RP2040)とと拡張基板TheExtendを介して、M5Core2とI2C通信で接続する<br>
- **[Structure_2]**:USBシリアル/Bluetooth及びRawHIDを使用し直接PCと接続し、M5Core2をスタンドアロンで活用する<br>

なお、Structure_2ではPCのローカル環境に専用アプリ[**typingbridge.py**](https://github.com/YMG-CODE/M5stack/tree/main/TypingBridge)をDLすることで、キーボードの入力情報をM5Core2へ送信します。

<img width="1039" height="582" alt="image" src="https://github.com/user-attachments/assets/a4b155ae-c753-415b-9e8e-1a116905916d" />
<img width="928" height="525" alt="image" src="https://github.com/user-attachments/assets/7ee7e73e-b908-4569-80b4-2992e57cb948" />

**<操作方法>**
- 起動後に接続モード選択(画面下部のABCボタンで選択)
  - A:USB/BTを使用する場合はtypingbridge.pyの以下ドキュメントをご確認下さい。
- **ボタン説明**
  - **Aボタン**
      - 短押し:Solenoidテスト
  - **Bボタン**
      - 短押し:Solenoidテスト
  - **Cボタン**
      - 短押し:Solenoidテスト(fastmode)
  - **画面タップ**
      - 長押し:SettingモードのOn(ABC何れかの短押しでSolenoid画面へ復帰)
- **Settingモード説明**
  - **概要**
    - 画面タップでスライダーを調整します。設定値は電源切断後も保存されます。
  - **Vibration**
    - バイブレーションの強度を変更します。On/Offと表示がある箇所をタップすると設定値に関わらず、On/Offが切り替え可能です。
  - **Tone**
    - 音程を変更します。
  - **Volume**
    - 音量を変更します(デバイスのスピーカー保護のため80%でリミッターをかけています)。<br><br>
<img width="475" height="454" alt="image" src="https://github.com/user-attachments/assets/14d7f439-c03f-4847-9b02-732f448b5dec" /><br>
<img width="476" height="463" alt="image" src="https://github.com/user-attachments/assets/53da7ece-9037-42f0-ab4f-b621194e3a4c" />


    
