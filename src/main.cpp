#include <iostream>
#include <memory>

#include "../hdr/command.h"
#include "../hdr/DaVinciDerivatives.h"

int main(int argc, char *argv[])
{
    std::unique_ptr<DaVinciOrderMatchingEngine> e = std::make_unique<DaVinciOrderMatchingEngine>();
    e->execute();
    return 0;
}
