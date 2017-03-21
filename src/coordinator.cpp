#include <string>
#include <thread>
#include <unordered_map>

#include <nanomsg/pipeline.h>
#include <nanomsg/pubsub.h>
#include <nanomsg/reqrep.h>

#include <meow/intrusive_ptr.hpp>

#include "pinba/globals.h"
#include "pinba/repacker.h"
#include "pinba/coordinator.h"
#include "pinba/report.h"

#include "pinba/nmsg_socket.h"
#include "pinba/nmsg_ticker.h"
#include "pinba/nmsg_poller.h"

////////////////////////////////////////////////////////////////////////////////////////////////
namespace { namespace aux {
////////////////////////////////////////////////////////////////////////////////////////////////

	struct report_host_conf_t
	{
		std::string name;
		std::string thread_name;

		std::string nn_reqrep;          // control messages and stuff
		std::string nn_shutdown;        // shutdown message

		std::string nn_packets;         // get packet_batch_ptr from this endpoint as fast as possible (SUB, pair to coodinator PUB)
		size_t      nn_packets_buffer;  // NN_RCVBUF on nn_packets
	};

	using report_host_call_func_t = std::function<void(report_t*)>;

	struct report_host_t
	{
		virtual ~report_host_t() {}
		virtual void startup(report_ptr) = 0;
		virtual void shutdown() = 0;

		virtual void process_batch(packet_batch_ptr) = 0;
		virtual void call_with_report(report_host_call_func_t const&) = 0;
	};
	typedef std::unique_ptr<report_host_t> report_host_ptr;

	struct report_host_req_t : public nmsg_message_t
	{
		report_host_call_func_t func;

		template<class Function>
		report_host_req_t(Function const& f)
			: func(f)
		{
		}
	};
	typedef boost::intrusive_ptr<report_host_req_t> report_host_req_ptr;

	struct report_host_result_t : public nmsg_message_t
	{};
	typedef boost::intrusive_ptr<report_host_result_t> report_host_result_ptr;

	struct report_host___new_thread_t : public report_host_t
	{
		pinba_globals_t        *globals_;
		report_host_conf_t     conf_;

		std::thread            t_;

		nmsg_socket_t          packets_send_sock_;
		nmsg_socket_t          packets_recv_sock_;

		nmsg_socket_t          reqrep_sock_;
		nmsg_socket_t          shutdown_sock_;
		nmsg_ticker_chan_ptr   ticker_chan_;

		report_ptr             report_;
		uint64_t               packets_received;

	public:

		report_host___new_thread_t(pinba_globals_t *globals, report_host_conf_t const& conf)
			: globals_(globals)
			, conf_(conf)
			, packets_received(0)
		{
			// packets_sock_
			// 	.open(AF_SP, NN_SUB)
			// 	.set_option(NN_SUB, NN_SUB_SUBSCRIBE, "")
			// 	.set_option(NN_SOL_SOCKET, NN_RCVBUF, sizeof(packet_batch_ptr) * conf_.nn_packets_buffer, "report_handler_t::in_sock")
			// 	.connect(conf_.nn_packets);

			packets_send_sock_
				.open(AF_SP, NN_PUSH)
				.bind(conf_.nn_packets);

			packets_recv_sock_
				.open(AF_SP, NN_PULL)
				.set_option(NN_SOL_SOCKET, NN_RCVBUF, sizeof(packet_batch_ptr) * conf_.nn_packets_buffer, ff::fmt_str("{0}/in_sock", conf_.name))
				.connect(conf_.nn_packets);

			reqrep_sock_
				.open(AF_SP, NN_REP)
				.bind(conf_.nn_reqrep);

			shutdown_sock_
				.open(AF_SP, NN_REP)
				.bind(conf_.nn_shutdown);
		}

	public:

		virtual void startup(report_ptr incoming_report) override
		{
			if (report_)
				throw std::logic_error(ff::fmt_str("report handler {0} is already started", conf_.name));

			report_ = move(incoming_report);

			//

			auto const *rinfo        = report_->info();
			auto const tick_interval = rinfo->time_window / rinfo->tick_count;

			ticker_chan_ = globals_->ticker()->subscribe(tick_interval, conf_.name);

			std::thread t([this]()
			{
				{
					pthread_setname_np(pthread_self(), conf_.thread_name.c_str());
				}

				report_->ticks_init(os_unix::clock_monotonic_now());

				nmsg_poller_t poller;
				poller
					.read(*ticker_chan_, [this](nmsg_ticker_chan_t& chan, timeval_t now)
					{
						chan.recv();
						report_->tick_now(now);
					})
					.read_sock(*packets_recv_sock_, [this](timeval_t now)
					{
						auto const batch = packets_recv_sock_.recv<packet_batch_ptr>();
						packets_received += batch->packet_count;

						report_->add_multi(batch->packets, batch->packet_count);
					})
					.read_sock(*reqrep_sock_, [this](timeval_t now)
					{
						auto const req = reqrep_sock_.recv<report_host_req_ptr>();
						req->func(report_.get());
						reqrep_sock_.send(meow::make_intrusive<report_host_result_t>());
					})
					.read_sock(*shutdown_sock_, [this, &poller](timeval_t)
					{
						shutdown_sock_.recv<int>();
						poller.set_shutdown_flag(); // exit loop() after this iteration
						shutdown_sock_.send(1);
					})
					.loop();

				// unsub from ticker
				// this is required to allow re-creation of report host with same name later
				// (due to ticker channels having names that must be unique within nanomsg)
				globals_->ticker()->unsubscribe(ticker_chan_);
			});

			t_ = move(t);
		}

		virtual void process_batch(packet_batch_ptr batch) override
		{
			bool const success = packets_send_sock_.send_message(batch, NN_DONTWAIT);
			if (!success)
			{
				// TODO: increment stats, etc.
			}
		}

		virtual void call_with_report(std::function<void(report_t*)> const& func) override
		{
			nmsg_socket_t sock;
			sock.open(AF_SP, NN_REQ).connect(conf_.nn_reqrep);

			sock.send_message(meow::make_intrusive<report_host_req_t>(func));
			sock.recv<report_host_result_ptr>();
		}

		virtual void shutdown() override
		{
			nmsg_socket_t sock;
			sock.open(AF_SP, NN_REQ).connect(conf_.nn_shutdown);

			sock.send(1);
			sock.recv<int>();

			t_.join();
		}
	};

////////////////////////////////////////////////////////////////////////////////////////////////

	struct coordinator_impl_t : public coordinator_t
	{
		coordinator_impl_t(pinba_globals_t *globals, coordinator_conf_t *conf)
			: globals_(globals)
			, conf_(conf)
		{
			in_sock_ = nmsg_socket(AF_SP, NN_PULL);
			if (conf_->nn_input_buffer > 0)
				in_sock_.set_option(NN_SOL_SOCKET, NN_RCVBUF, conf_->nn_input_buffer * sizeof(packet_batch_ptr), conf_->nn_input);

			control_sock_ = nmsg_socket(AF_SP, NN_REP);
			control_sock_.bind(conf_->nn_control.c_str());

			report_sock_ = nmsg_socket(AF_SP, NN_PUB);
			report_sock_.bind(conf_->nn_report_output.c_str());
		}

		virtual void startup() override
		{
			in_sock_.connect(conf_->nn_input.c_str());

			std::thread t([this]()
			{
				this->worker_thread();
			});
			t_ = move(t);
		}

		virtual void shutdown() override
		{
			this->request(meow::make_intrusive<coordinator_request___shutdown_t>());
			t_.join();
		}

		virtual coordinator_response_ptr request(coordinator_request_ptr req) override
		{
			nmsg_socket_t sock;
			sock.open(AF_SP, NN_REQ).connect(conf_->nn_control);

			sock.send_message(req);
			return sock.recv<coordinator_response_ptr>();
		}

	private:

		coordinator_request_ptr control_recv()
		{
			return control_sock_.recv<coordinator_request_ptr>();
		}

		void control_send(coordinator_response_ptr response)
		{
			control_sock_.send_message(response);
		}

		template<class T, class... A>
		void control_send(A&&... args)
		{
			this->control_send(meow::make_intrusive<T>(std::forward<A>(args)...));
		}

		template<class... A>
		void control_send_generic(A&&... args)
		{
			this->control_send<coordinator_response___generic_t>(std::forward<A>(args)...);
		}

		void worker_thread()
		{
			{
				std::string const thr_name = ff::fmt_str("coordinator");
				pthread_setname_np(pthread_self(), thr_name.c_str());
			}

			auto const tick_chan = globals_->ticker()->subscribe(1000 * d_millisecond, "coordinator_thread");

			nmsg_poller_t poller;
			poller
				.read(*tick_chan, [this](nmsg_ticker_chan_t& chan, timeval_t now)
				{
					chan.recv(); // MUST do this, or chan will stay readable
				})
				.read_sock(*in_sock_, [this](timeval_t now)
				{
					auto batch = in_sock_.recv<packet_batch_ptr>();

					// relay the batch to all reports
					// maybe move this to a separate thread?
					for (auto& report_host : report_hosts_)
					{
						report_host.second->process_batch(batch);
					}
#if 0
					// FIXME: this is fundamentally broken with PUB/SUB sadly
					// since we have no idea if the report handler is slow, and in that case messages will be dropped
					// and ref counts will not be decremented and memory will leak and we won't have any stats about that either
					// (we also have no need for the pub/sub routing part at the moment and probably won't need it ever)
					// so should probably reimplement this with just a loop sending things to all reports
					// (might also try avoid blocking with threads/polls, but this might get too complicated)
					// and _add_ref() should be called only if send was successful (with NN_DONTWAIT ofc), to avoid the need to call _release()
					// note that this requires multiple calls to _add_ref() instead of single one
					{
						for (size_t i = 0, i_end = report_hosts_.size(); i < i_end; i++)
							intrusive_ptr_add_ref(batch.get());

						report_sock_.send_ex(batch, 0);
					}
#endif
				})
				.read_sock(*control_sock_, [this, &poller](timeval_t now)
				{
					auto const req = this->control_recv();

					try
					{
						switch (req->type)
						{
							case COORDINATOR_REQ__CALL:
							{
								auto *r = static_cast<coordinator_request___call_t*>(req.get());
								r->func(this);
								this->control_send_generic(COORDINATOR_STATUS__OK);
							}
							break;

							case COORDINATOR_REQ__SHUTDOWN:
							{
								auto *r = static_cast<coordinator_request___shutdown_t*>(req.get());

								// shutdown all reports
								for (auto& report_host : report_hosts_)
									report_host.second->shutdown();

								// stop poll loop and reply
								poller.set_shutdown_flag();
								this->control_send_generic(COORDINATOR_STATUS__OK);
							}
							break;

							case COORDINATOR_REQ__ADD_REPORT:
							{
								auto *r = static_cast<coordinator_request___add_report_t*>(req.get());

								auto const report_name = r->report->name().str();
								auto const thread_id   = report_hosts_.size();
								auto const thr_name    = ff::fmt_str("rh/{0}", thread_id);
								auto const rh_name     = ff::fmt_str("rh/{0}/{1}", thread_id, report_name);

								report_host_conf_t const rh_conf = {
									.name              = rh_name,
									.thread_name       = thr_name,
									.nn_reqrep         = ff::fmt_str("inproc://{0}/control", rh_name),
									.nn_shutdown       = ff::fmt_str("inproc://{0}/shutdown", rh_name),
									.nn_packets        = ff::fmt_str("inproc://{0}/packets", rh_name),
									.nn_packets_buffer = conf_->nn_report_output_buffer,
								};

								auto rh = meow::make_unique<report_host___new_thread_t>(globals_, rh_conf);
								rh->startup(move(r->report));

								report_hosts_.emplace(report_name, move(rh));

								this->control_send_generic(COORDINATOR_STATUS__OK);
							}
							break;

							case COORDINATOR_REQ__DELETE_REPORT:
							{
								auto *r = static_cast<coordinator_request___delete_report_t*>(req.get());

								auto const it = report_hosts_.find(r->report_name);
								if (it == report_hosts_.end())
								{
									this->control_send_generic(COORDINATOR_STATUS__ERROR, ff::fmt_str("unknown report: {0}", r->report_name));
									return;
								}

								report_host_t *host = it->second.get();
								host->shutdown(); // waits for host to completely shut itself down

								auto const n_erased = report_hosts_.erase(r->report_name);
								assert(n_erased == 1);

								this->control_send_generic(COORDINATOR_STATUS__OK);
							}
							break;

							case COORDINATOR_REQ__GET_REPORT_SNAPSHOT:
							{
								auto *r = static_cast<coordinator_request___get_report_snapshot_t*>(req.get());

								auto const it = report_hosts_.find(r->report_name);
								if (it == report_hosts_.end())
								{
									this->control_send_generic(COORDINATOR_STATUS__ERROR, ff::fmt_str("unknown report: {0}", r->report_name));
									return;
								}

								report_host_t *host = it->second.get();

								report_snapshot_ptr snapshot;
								host->call_with_report([&](report_t *report)
								{
									snapshot = report->get_snapshot();
								});

								auto response = meow::make_intrusive<coordinator_response___report_snapshot_t>();
								response->snapshot = move(snapshot);

								this->control_send(response);
							}
							break;

							default:
								throw std::runtime_error(ff::fmt_str("unknown coordinator_control_request type: {0}", req->type));
								break;
						}
					}
					catch (std::exception const& e)
					{
						this->control_send_generic(COORDINATOR_STATUS__ERROR, e.what());
					}
				})
				.loop();

			globals_->ticker()->unsubscribe(tick_chan);
		}

	private:
		pinba_globals_t     *globals_;
		coordinator_conf_t  *conf_;

		// report_name -> report_host
		using report_host_map_t = std::unordered_map<std::string, report_host_ptr>;
		report_host_map_t report_hosts_;

		nmsg_socket_t    in_sock_;
		nmsg_socket_t    control_sock_;
		nmsg_socket_t    report_sock_;

		std::thread      t_;
	};

////////////////////////////////////////////////////////////////////////////////////////////////
}} // namespace { namespace aux {
////////////////////////////////////////////////////////////////////////////////////////////////

coordinator_ptr create_coordinator(pinba_globals_t *globals, coordinator_conf_t *conf)
{
	return meow::make_unique<aux::coordinator_impl_t>(globals, conf);
}
