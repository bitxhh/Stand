#include "Application/Application.h"
#include "Hardware/LimeManager.h"

int main(int argc, char* argv[]) {
    LimeManager manager;
    Application app(argc, argv, manager);

    return app.run();
}
