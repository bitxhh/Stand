#include "Application/Application.h"
#include "Hardware/LimeDeviceManager.h"

int main(int argc, char* argv[]) {
    LimeDeviceManager manager;
    Application app(argc, argv, manager);

    return app.run();
}
