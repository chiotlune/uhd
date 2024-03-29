//
// Copyright 2011-2013 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "tx_dsp_core_3000.hpp"
#include <uhd/types/dict.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/algorithm.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/math/special_functions/round.hpp>
#include <boost/math/special_functions/sign.hpp>
#include <boost/thread/thread.hpp> //sleep
#include <algorithm>
#include <cmath>

#define REG_DSP_TX_FREQ          _dsp_base + 0
#define REG_DSP_TX_SCALE_IQ      _dsp_base + 4
#define REG_DSP_TX_INTERP        _dsp_base + 8

template <class T> T ceil_log2(T num){
    return std::ceil(std::log(num)/std::log(T(2)));
}

using namespace uhd;

class tx_dsp_core_3000_impl : public tx_dsp_core_3000{
public:
    tx_dsp_core_3000_impl(
        wb_iface::sptr iface,
        const size_t dsp_base
    ):
        _iface(iface), _dsp_base(dsp_base)
    {
        //init to something so update method has reasonable defaults
        _scaling_adjustment = 1.0;
        _dsp_extra_scaling = 1.0;
        this->set_tick_rate(1.0);
    }

    void set_tick_rate(const double rate){
        _tick_rate = rate;
    }

    void set_link_rate(const double rate){
        //_link_rate = rate/sizeof(boost::uint32_t); //in samps/s
        _link_rate = rate/sizeof(boost::uint16_t); //in samps/s (allows for 8sc)
    }

    uhd::meta_range_t get_host_rates(void){
        meta_range_t range;
        for (int rate = 512; rate > 256; rate -= 4){
            range.push_back(range_t(_tick_rate/rate));
        }
        for (int rate = 256; rate > 128; rate -= 2){
            range.push_back(range_t(_tick_rate/rate));
        }
        for (int rate = 128; rate >= int(std::ceil(_tick_rate/_link_rate)); rate -= 1){
            range.push_back(range_t(_tick_rate/rate));
        }
        return range;
    }

    double set_host_rate(const double rate){
        const size_t interp_rate = boost::math::iround(_tick_rate/this->get_host_rates().clip(rate, true));
        size_t interp = interp_rate;

        //determine which half-band filters are activated
        int hb0 = 0, hb1 = 0;
        if (interp % 2 == 0){
            hb0 = 1;
            interp /= 2;
        }
        if (interp % 2 == 0){
            hb1 = 1;
            interp /= 2;
        }

        _iface->poke32(REG_DSP_TX_INTERP, (hb1 << 9) | (hb0 << 8) | (interp & 0xff));

        if (interp > 1 and hb0 == 0 and hb1 == 0)
        {
            UHD_MSG(warning) << boost::format(
                "The requested interpolation is odd; the user should expect CIC rolloff.\n"
                "Select an even interpolation to ensure that a halfband filter is enabled.\n"
                "interpolation = dsp_rate/samp_rate -> %d = (%f MHz)/(%f MHz)\n"
            ) % interp_rate % (_tick_rate/1e6) % (rate/1e6);
        }

        // Calculate CIC interpolation (i.e., without halfband interpolators)
        // Calculate closest multiplier constant to reverse gain absent scale multipliers
        const double rate_pow = std::pow(double(interp & 0xff), 3);
        _scaling_adjustment = std::pow(2, ceil_log2(rate_pow))/(1.65*rate_pow);
        this->update_scalar();

        return _tick_rate/interp_rate;
    }

    void update_scalar(void){
        const double factor = 1.0 + std::max(ceil_log2(_scaling_adjustment), 0.0);
        const double target_scalar = (1 << 17)*_scaling_adjustment/_dsp_extra_scaling/factor;
        const boost::int32_t actual_scalar = boost::math::iround(target_scalar);
        _fxpt_scalar_correction = target_scalar/actual_scalar*factor; //should be small
        _iface->poke32(REG_DSP_TX_SCALE_IQ, actual_scalar);
    }

    double get_scaling_adjustment(void){
        return _fxpt_scalar_correction*_host_extra_scaling*32767.;
    }

    double set_freq(const double freq_){
        //correct for outside of rate (wrap around)
        double freq = std::fmod(freq_, _tick_rate);
        if (std::abs(freq) > _tick_rate/2.0)
            freq -= boost::math::sign(freq)*_tick_rate;

        //calculate the freq register word (signed)
        UHD_ASSERT_THROW(std::abs(freq) <= _tick_rate/2.0);
        static const double scale_factor = std::pow(2.0, 32);
        const boost::int32_t freq_word = boost::int32_t(boost::math::round((freq / _tick_rate) * scale_factor));

        //update the actual frequency
        const double actual_freq = (double(freq_word) / scale_factor) * _tick_rate;

        _iface->poke32(REG_DSP_TX_FREQ, boost::uint32_t(freq_word));

        return actual_freq;
    }

    uhd::meta_range_t get_freq_range(void){
        return uhd::meta_range_t(-_tick_rate/2, +_tick_rate/2, _tick_rate/std::pow(2.0, 32));
    }

    void setup(const uhd::stream_args_t &stream_args){

        //unsigned format_word = 0;
        if (stream_args.otw_format == "sc16"){
            //format_word = 0;
            _dsp_extra_scaling = 1.0;
            _host_extra_scaling = 1.0;
        }
        /*
        else if (stream_args.otw_format == "sc8"){
            format_word = (1 << 0);
            double peak = stream_args.args.cast<double>("peak", 1.0);
            peak = std::max(peak, 1.0/256);
            _host_extra_scaling = 1.0/peak/256;
            _dsp_extra_scaling = 1.0/peak;
        }
        else throw uhd::value_error("USRP TX cannot handle requested wire format: " + stream_args.otw_format);
        */

        _host_extra_scaling /= stream_args.args.cast<double>("fullscale", 1.0);

        this->update_scalar();
    }

private:
    wb_iface::sptr _iface;
    const size_t _dsp_base;
    double _tick_rate, _link_rate;
    double _scaling_adjustment, _dsp_extra_scaling, _host_extra_scaling, _fxpt_scalar_correction;
};

tx_dsp_core_3000::sptr tx_dsp_core_3000::make(wb_iface::sptr iface, const size_t dsp_base)
{
    return sptr(new tx_dsp_core_3000_impl(iface, dsp_base));
}
