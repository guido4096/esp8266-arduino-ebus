#ifndef _ARBITRATION_H_
#define _ARBITRATION_H_

#include "ebusstate.hpp"

// Implements the arbitration algorithm. Uses the state of the bus to decide what to do.
// Typical usage:
// - try to start the arbitration with "start" method
// - pass each received value on the bus to the "data" method
//   which will then tell you what the state of the arbitration is
class Arbitration
{
    public:
        enum state {none,  // no arbitration ongoing/not yet completed
                    arbitrating, // arbitration ongoing
                    won,   // won
                    lost,  // lost
                    error  // error
                    };

        Arbitration()
        : _arbitrating(false)
        , _participateSecond(false)
        , _arbitration_address(0)
        {}
    // Try to start arbitration for the specified master.
    // Return values:
    // - false : arbitration not started. Possible reasons:
    //           + the bus is not in a state that allows to start arbitration
    //           + another arbitration is already ongoing
    //           + the master address is SYN
    // = true  : arbitration started. Make sure to pass all bus data to this object through the "data" method
    bool               start  (EBusState& busstate, uint8_t master);

    // A symbol was received on the bus, what does this do to the arbitration state?
    // Return values:
    // - see description of state enum value
    Arbitration::state data   (EBusState& busstate, uint8_t symbol);

    private:
        bool    _arbitrating;
        bool    _participateSecond;
        uint8_t _arbitration_address;
};

#endif
