# Loyalty Card Reader

A Win32 x64 console app (Visual Studio 2026, C++20) that lets customers sign up for a
loyalty program by tapping or inserting a payment card. It reads the cardholder name
from the card's EMV application, so nobody has to type it in.

## How it works

1. Lists every PC/SC smart card reader on the system.
2. Starts one worker thread per reader. Each worker prints in its own ANSI color.
3. Each worker waits for a card (`SCardGetStatusChange`).
4. When a card arrives, the worker tries to SELECT an EMV application, in this order:

   | Priority | RID          | Scheme           |
   |----------|--------------|------------------|
   | 1        | `A000000003` | Visa             |
   | 2        | `A000000004` | Mastercard       |
   | 3        | `A000000025` | American Express |
   | 4        | `A000000152` | Discover         |
   | 5        | `A000000065` | JCB              |
   | 6        | `A000000333` | UnionPay         |
   | 7        | `A000000524` | RuPay            |

5. If none of them can be selected, the worker prints a **REJECTED** message.
6. Otherwise it reads the name with these commands:
   * `GET PROCESSING OPTIONS`, with the card's PDOL filled in with zero-amount terminal values.
   * `READ RECORD` for each entry in the AFL. The worker stops once it has both the name and the BIN.
   * It looks for tag `5F20` (cardholder name) first, then `9F0B` (extended name), then the
     name field in Track 1 (`56`). The card stores names as `SURNAME/GIVEN`; the app
     prints them as `GIVEN SURNAME`.
   * It reads the **BIN** (the first 6 digits of the card number, which identify the issuer)
     from the PAN (`5A`), or from the Track 2 data (`57` / `9F6B`) or Track 1 data (`56`).
     The app keeps only those 6 digits. It never stores or prints the full card number.
   * If the card rejects GPO, the worker tries reading records from SFI 1–10 directly.
7. Prints **ACCEPTED** with the full AID (from tag `84`), the BIN and the name.
8. Waits for the card to be removed, then waits for the next one.

## Build

Open `LoyaltyCardReader.slnx` in Visual Studio 2026 and build the `x64` configuration.
The project uses the v145 toolset and links `winscard.lib`.

## Run

```
LoyaltyCardReader.exe            # normal mode
LoyaltyCardReader.exe --verbose  # also show every APDU sent and received
```

Press Ctrl+C to exit.

### Verbose mode and sensitive data

Verbose mode prints each command (`>>`) and response (`<<`) together with the status word.
Before printing, it masks the values of the PAN (`5A`), Track 1/2 data (`56`, `57`, `9F6B`,
`9F1F`, `9F20`) and expiry date (`5F24`) as `**`. If a response can't be parsed as TLV,
the whole response is hidden. Outside of the BIN, the app never prints or stores the PAN.

## Things to know

* **Many contactless cards leave the name out.** For privacy, most cards issued in recent
  years return an empty or generic name (for example `" /"` or `VALUED CUSTOMER`) over NFC,
  and sometimes over contact too. In that case the app prints `Name: <not provided by this card>`.
  Plan a fallback, such as typing the name in.
* Readers plugged in after the app starts are not picked up. Restart the app to use them.
* Some readers show up as two readers (contact and contactless). Each one gets its own worker.
