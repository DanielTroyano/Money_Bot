# MoneyBot ESP32-S3 Firmware

A fun IoT device that celebrates sales with animated displays! When a payment is processed through your Stripe integration, MoneyBot plays a "cha-ching" animation with falling money tokens on its round LCD display.

## Hardware

- **MCU**: ESP32-S3 DevKitC-1
- **Display**: GC9A01 240x240 Round LCD (SPI)
- **LED**: WS2812 RGB LED (GPIO 38)

## Features

- 🤖 Animated robot face with glowing eyes
- 💰 "Money rain" animation on sale events
- 📶 Wi-Fi provisioning via QR code
- ☁️ AWS IoT Core MQTT connectivity (mTLS)
- 🔔 Real-time Stripe webhook integration

## Architecture

```
Stripe Payment → API Gateway → Lambda → AWS IoT Core → ESP32 MoneyBot
                                              ↓
                                    Topic: moneybot/<deviceId>/cmd
```

## Setup

### 1. Prerequisites

- ESP-IDF v5.0+ installed
- AWS IoT Core Thing created with certificate
- Stripe webhook configured to your Lambda

### 2. Certificates

Place the following files in `main/certs/`:

```
main/certs/
├── amazon_root_ca.pem      # AmazonRootCA1.pem from AWS
├── device_cert.pem.crt     # Device certificate from AWS IoT
└── private_key.pem.key     # Private key (KEEP SECRET!)
```

> ⚠️ **Security**: The `main/certs/` directory is in `.gitignore`. Never commit private keys!

### 3. AWS IoT Policy

Ensure your IoT Thing's policy allows:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:us-east-1:*:client/dev-001"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:us-east-1:*:topicfilter/moneybot/dev-001/*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": "arn:aws:iot:us-east-1:*:topic/moneybot/dev-001/*"
    }
  ]
}
```

### 4. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

### 5. Wi-Fi Provisioning

On first boot (or if stored WiFi fails):

1. MoneyBot displays a QR code on its screen
2. Download the **ESP SoftAP Provisioning** app ([iOS](https://apps.apple.com/app/esp-softap-provisioning/id1474040630) / [Android](https://play.google.com/store/apps/details?id=com.espressif.provsoftap))
3. Scan the QR code
4. Enter your WiFi credentials
5. MoneyBot connects and stores credentials in NVS

## Testing

### Test from AWS IoT Console

1. Go to **AWS IoT Core** → **Test** → **MQTT test client**
2. Publish to topic: `moneybot/dev-001/cmd`
3. Message payload:

```json
{
  "type": "sale",
  "status": "succeeded",
  "amount": 2000,
  "currency": "usd",
  "eventId": "evt_test123"
}
```

4. MoneyBot should play the sale animation! 🎉

### Test via Stripe CLI

If your full pipeline is configured:

```bash
stripe trigger invoice.payment_succeeded
```

This triggers your webhook → Lambda → IoT Core → MoneyBot.

### Manual MQTT Test (mosquitto)

```bash
mosquitto_pub \
  --cafile AmazonRootCA1.pem \
  --cert device_cert.pem.crt \
  --key private_key.pem.key \
  -h a3krir0duhayc0-ats.iot.us-east-1.amazonaws.com \
  -p 8883 \
  -t "moneybot/dev-001/cmd" \
  -m '{"type":"sale","status":"succeeded","amount":5000,"currency":"usd"}'
```

## LED Status Indicators

| Color    | State                            |
| -------- | -------------------------------- |
| 🔴 Red   | Disconnected / Starting up       |
| 🟡 Gold  | Wi-Fi provisioning mode          |
| 🔵 Cyan  | Wi-Fi connected, MQTT connecting |
| 🟢 Green | Fully connected to AWS IoT       |

## Connection Status (Antenna Ball)

The antenna ball on the robot face also indicates connection status:

- **Cyan**: Idle / Connecting
- **Green**: MQTT connected
- **Red**: Disconnected

## Configuration

Key settings in `main/main.c`:

| Constant                | Description                      | Default                 |
| ----------------------- | -------------------------------- | ----------------------- |
| `DEFAULT_DEVICE_ID`     | Device identifier                | `dev-001`               |
| `AWS_IOT_ENDPOINT`      | IoT Core ATS endpoint            | `a3krir0duhayc0-ats...` |
| `ANIMATION_DEBOUNCE_MS` | Min time between animations      | `1000`                  |
| `PROV_POP`              | Provisioning proof-of-possession | `abcd1234`              |

## Message Format

The device expects JSON messages with:

```json
{
  "type": "sale", // Required: must be "sale"
  "status": "succeeded", // Optional: defaults to succeeded
  "amount": 2000, // Optional: amount in cents
  "currency": "usd", // Optional: currency code
  "eventId": "evt_..." // Optional: for logging
}
```

## Troubleshooting

### TLS Handshake Fails

- Ensure system time is correct (SNTP sync)
- Verify certificates are valid and not expired
- Check IoT policy allows the client ID

### WiFi Won't Connect

- Reset provisioning: erase NVS flash with `idf.py erase-flash`
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

### No Animation on Message

- Check serial monitor for MQTT connection status
- Verify topic matches exactly: `moneybot/dev-001/cmd`
- Check JSON format (type must be "sale")

## Future Enhancements

- [ ] Multi-device support via DynamoDB
- [ ] Web-based provisioning portal
- [ ] Custom animation for different payment amounts
- [ ] OTA firmware updates

## License

MIT
