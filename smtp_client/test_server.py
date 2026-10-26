from aiosmtpd.controller import Controller


class DebugHandler:
    async def handle_DATA(self, server, session, envelope):
        print("From:", envelope.mail_from)
        print("To:", envelope.rcpt_tos)
        print("Message:")
        print(envelope.content.decode("utf-8", errors="replace"))
        print("-" * 40)
        return "250 Message accepted for testing"


controller = Controller(
    DebugHandler(),
    hostname="127.0.0.1",
    port=2525
)

controller.start()
print("Test SMTP server listening on 127.0.0.1:2525")

try:
    input("Press Enter to stop...\n")
finally:
    controller.stop()
