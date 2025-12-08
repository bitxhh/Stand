#include "Application/Application.h"
#include "LimeCore/LimeManager.h"

int main(int argc, char* argv[]) {
    LimeManager manager;
    Application app(argc, argv, manager);
    return app.run();
}
