#pragma once
#include "policy.h"
#include "rlop/rl/rl.h"
#include "rlop/rl/buffers.h"

namespace rlop {
    class SAC : public RL {
    public:
        SAC(
            Int learning_starts = 100,
            Int batch_size = 256,
            double lr = 3e-4,
            double tau = 0.005,
            double gamma = 0.99,
            double ent_coef = 1.0,
            bool auto_ent_coef = true,
            const std::optional<double>& target_entropy = std::nullopt,
            Int train_freq = 1,
            Int gradient_steps = 1,
            Int target_update_interval = 1,
            const std::string& output_path = ".",
            const torch::Device& device = torch::kCPU
        ) :
            RL(output_path, device),
            learning_starts_(learning_starts),
            batch_size_(batch_size),
            lr_(lr),
            tau_(tau),
            gamma_(gamma),
            ent_coef_(ent_coef),
            auto_ent_coef_(auto_ent_coef),
            train_freq_(train_freq),
            gradient_steps_(gradient_steps),
            target_update_interval_(target_update_interval)
        {
            if (target_entropy)
                target_entropy_ = *target_entropy;
            else
                auto_target_entropy_ = true;
        }

        virtual ~SAC() = default;

        virtual std::unique_ptr<ReplayBuffer> MakeReplayBuffer() const = 0; 

        virtual std::unique_ptr<SACActor> MakeActor() const = 0;

        virtual std::unique_ptr<SACCritic> MakeCritic() const = 0;

        virtual torch::Tensor SampleAction() = 0;

        virtual torch::Tensor ResetEnv() = 0;

        virtual std::array<torch::Tensor, 3> Step(const torch::Tensor& action) = 0;
        
        virtual void Reset() override {
            RL::Reset();
            replay_buffer_ = MakeReplayBuffer();
            actor_ = MakeActor();
            actor_->to(device_);
            critic_ = MakeCritic();
            critic_->to(device_);
            critic_->Reset();
            critic_target_ = MakeCritic();
            critic_target_->to(device_);
            critic_target_->Reset();
            critic_target_->eval();
            torch_utils::CopyStateDict(*critic_, critic_target_.get());
            actor_optimizer_ = MakeActorOptimizer();
            critic_optimizer_ = MakeCriticOptimizer();
            if (auto_ent_coef_) {
                log_ent_coef_ = torch::log(torch::ones(1, device_) * ent_coef_).set_requires_grad(true);
                ent_coef_optimizer_ = MakeEntropyOptimizer();
                if (auto_target_entropy_) {
                    target_entropy_ = -1.0;
                    for(Int size : replay_buffer_->action_sizes()) {
                        target_entropy_ *= size;
                    }
                }
            }
            else
                ent_coef_tensor_ = torch::tensor(ent_coef_, device_);
            observation_ = ResetEnv();
        }

        virtual void RegisterLogItems() {
            log_items_["ent_coef"] = torch::Tensor();
            log_items_["actor_loss"] = torch::Tensor();
            log_items_["critic_loss"] = torch::Tensor();
            log_items_["q_value"] = torch::Tensor();
            log_items_["reward"] = torch::Tensor();
            if (auto_ent_coef_)
                log_items_["ent_coef_loss"] = torch::Tensor();
        }

        virtual void CollectRollouts() override {
            actor_->eval();
            critic_->eval();
            torch::NoGradGuard no_grad;
            if (num_iters_ == 0) {
                for (Int step = 0; step < learning_starts_; ++step) {
                    torch::Tensor action = SampleAction();
                    auto [next_observation, reward, done] = Step(action);
                    replay_buffer_->Add(observation_, action, next_observation, reward, done);
                    observation_ = next_observation;
                }
            }
            for (Int step = 0; step < train_freq_; ++step) {
                torch::Tensor action = actor_->PredictAction(observation_.to(device_));
                auto [next_observation, reward, done] = Step(action);
                replay_buffer_->Add(observation_, action, next_observation, reward, done);
                observation_ = next_observation;
                time_steps_ += NumEnvs();
            }
        }

        virtual std::array<torch::Tensor, 2> Predict(const torch::Tensor& observation, bool deterministic = false, const torch::Tensor& state = torch::Tensor(), const torch::Tensor& episode_start = torch::Tensor()) {
            return actor_->Predict(observation.to(device_), deterministic, state, episode_start);
        }
    
        virtual void Train() override {
            actor_->train();
            critic_->train();
            std::vector<torch::Tensor> ent_coef_list;
            std::vector<torch::Tensor> actor_loss_list;
            std::vector<torch::Tensor> critic_loss_list;
            std::vector<torch::Tensor> ent_coef_loss_list;
            std::vector<torch::Tensor> q_value_list;
            std::vector<torch::Tensor> reward_list;
            ent_coef_list.reserve(gradient_steps_);
            actor_loss_list.reserve(gradient_steps_);
            critic_loss_list.reserve(gradient_steps_);
            ent_coef_loss_list.reserve(gradient_steps_);
            q_value_list.reserve(gradient_steps_);
            reward_list.reserve(gradient_steps_);
            for (Int step=0; step<gradient_steps_; ++step) {
                auto batch = replay_buffer_->Sample(batch_size_).to(device_);
                auto [actions_pi, log_prob] = actor_->PredictActionLogProb(batch.observation);
                torch::Tensor ent_coef;
                torch::Tensor ent_coef_loss;
                if (ent_coef_optimizer_ != nullptr && log_ent_coef_.defined()) {
                    ent_coef = torch::exp(log_ent_coef_.detach());
                    ent_coef_loss = -(log_ent_coef_ * (log_prob + target_entropy_).detach()).mean();
                    ent_coef_loss_list.push_back(ent_coef_loss.detach()); 
                }
                else 
                    ent_coef = ent_coef_tensor_;
                ent_coef_list.push_back(ent_coef.detach());
                if (ent_coef_optimizer_ != nullptr && log_ent_coef_.defined()) {
                    ent_coef_optimizer_->zero_grad();
                    ent_coef_loss.backward();
                    ent_coef_optimizer_->step(); 
                } 
                torch::Tensor target_q_values;
                {
                    torch::NoGradGuard no_grad;
                    auto [next_actions, next_log_prob] = actor_->PredictActionLogProb(batch.next_observation);
                    torch::Tensor next_q_values = torch::stack(critic_target_->Forward(batch.next_observation, next_actions), 1);
                    next_q_values = std::get<0>(next_q_values.min(1)) - ent_coef * next_log_prob;
                    target_q_values = batch.reward + (1 - batch.done) * gamma_ * next_q_values;
                }
                auto current_q_values = critic_->Forward(batch.observation, batch.action);
                torch::Tensor min_q_value = std::get<0>(torch::stack(current_q_values, 1).detach().min(1));
                q_value_list.push_back(std::move(min_q_value));
                reward_list.push_back(batch.reward.detach());
                torch::Tensor critic_loss = torch::tensor(0.0, device_);
                for (const auto& current_q : current_q_values) {
                    critic_loss += torch::mse_loss(current_q, target_q_values);
                }
                critic_loss /= double(current_q_values.size());

                critic_loss_list.push_back(critic_loss.detach());
                critic_optimizer_->zero_grad();
                critic_loss.backward();
                critic_optimizer_->step();

                torch::Tensor q_values_pi = torch::stack(critic_->Forward(batch.observation, actions_pi), 1);
                torch::Tensor min_qf_pi = std::get<0>(q_values_pi.min(1));
                torch::Tensor actor_loss = (ent_coef * log_prob - min_qf_pi).mean();
                actor_loss_list.push_back(actor_loss.detach());
                actor_optimizer_->zero_grad();
                actor_loss.backward();
                actor_optimizer_->step();

                if (step % target_update_interval_ == 0) {
                    auto [ params_names, params ] = torch_utils::GetParameters(*critic_);
                    auto [ target_params_names, target_params ] = torch_utils::GetParameters(*critic_target_);
                    auto [ buffer_names, buffers ] = torch_utils::GetBuffers(*critic_);
                    auto [ target_buffer_names, target_buffers ] = torch_utils::GetBuffers(*critic_target_);
                    torch_utils::PolyakUpdate(params, target_params, tau_);
                    torch_utils::PolyakUpdate(buffers, target_buffers, 1.0);
                }
            }
            num_updates_ += gradient_steps_;
            if (!log_items_.empty()) {
                auto it = log_items_.find("num_updates");
                if (it != log_items_.end()) 
                    it->second = torch::tensor(num_updates_);
                it = log_items_.find("ent_coef");
                if (it != log_items_.end()) 
                    it->second = torch::stack(ent_coef_list).mean();
                it = log_items_.find("actor_loss");
                if (it != log_items_.end()) 
                    it->second  = torch::stack(actor_loss_list).mean();
                it = log_items_.find("critic_loss");
                if (it != log_items_.end()) 
                    it->second  = torch::stack(critic_loss_list).mean();
                it = log_items_.find("ent_coef_loss");
                if (it != log_items_.end()) 
                    it->second  = torch::stack(ent_coef_loss_list).mean();
                it = log_items_.find("q_value");
                if (it != log_items_.end()) 
                    it->second  = torch::stack(q_value_list).mean();
                it = log_items_.find("reward");
                if (it != log_items_.end()) 
                    it->second  = torch::stack(reward_list).mean();
            }
        }

        virtual void LoadArchive(torch::serialize::InputArchive* archive, const std::unordered_set<std::string>& names) override {
            if (names.count("all") || names.count("actor")) {
                torch::serialize::InputArchive actor_archive;
                if (archive->try_read("actor", actor_archive)) {
                    archive->read("actor", actor_archive);
                    actor_->load(actor_archive);
                }
            }
            if (names.count("all") || names.count("critic")) {
                torch::serialize::InputArchive critic_archive;
                if (archive->try_read("critic", critic_archive)) {
                    archive->read("critic", critic_archive);
                    critic_->load(critic_archive);
                }
            }
            if (names.count("all") || names.count("critic_target")) {
                 torch::serialize::InputArchive critic_target_archive;
                if (archive->try_read("critic_target", critic_target_archive)) {
                    archive->read("critic_target", critic_target_archive);
                    critic_target_->load(critic_target_archive);
                }
            }
            if (names.count("all") || names.count("actor_optimizer")) {
                torch::serialize::InputArchive actor_optimizer_archive;
                if (archive->try_read("actor_optimizer", actor_optimizer_archive)) {
                    archive->read("actor_optimizer", actor_optimizer_archive);
                    actor_optimizer_->load(actor_optimizer_archive);
                }
            }
            if (names.count("all") || names.count("critic_optimizer")) {
                torch::serialize::InputArchive critic_optimizer_archive;
                if (archive->try_read("critic_optimizer", critic_optimizer_archive)) {
                    archive->read("critic_optimizer", critic_optimizer_archive);
                    critic_optimizer_->load(critic_optimizer_archive);
                }
            }
            if ((names.count("all") || names.count("ent_coef_optimizer")) && ent_coef_optimizer_ != nullptr) {
                torch::serialize::InputArchive ent_coef_optimizer_archive;
                if (archive->try_read("ent_coef_optimizer", ent_coef_optimizer_archive)) {
                    archive->read("ent_coef_optimizer", ent_coef_optimizer_archive);
                    ent_coef_optimizer_->load(ent_coef_optimizer_archive);
                }
            }
            if (names.count("all") || names.count("hparams")) {
                torch::Tensor tensor;
                if (archive->try_read("learning_starts", tensor))
                    learning_starts_ = tensor.item<Int>();
                tensor = torch::Tensor();
                if (archive->try_read("batch_size", tensor))
                    batch_size_ = tensor.item<Int>();
                tensor = torch::Tensor();
                if (archive->try_read("lr", tensor))
                    lr_ = tensor.item<double>();
                tensor = torch::Tensor();
                if (archive->try_read("tau", tensor))
                    tau_ = tensor.item<double>();
                tensor = torch::Tensor();
                if (archive->try_read("gamma", tensor))
                    gamma_ = tensor.item<double>();
                tensor = torch::Tensor();
                if (archive->try_read("ent_coef", tensor))
                    ent_coef_ = tensor.item<double>();
                tensor = torch::Tensor();
                if (archive->try_read("target_entropy", tensor))
                    target_entropy_ = tensor.item<double>();
                tensor = torch::Tensor();
                if (archive->try_read("auto_ent_coef", tensor))
                    auto_ent_coef_ = tensor.item<bool>();
                tensor = torch::Tensor();
                if (archive->try_read("train_freq", tensor))
                    train_freq_ = tensor.item<Int>();
                tensor = torch::Tensor();
                if (archive->try_read("gradient_steps", tensor))
                    gradient_steps_ = tensor.item<Int>();
                tensor = torch::Tensor();
                if (archive->try_read("target_update_interval", tensor))
                    target_update_interval_ = tensor.item<Int>();
                
                archive->try_read("log_ent_coef", log_ent_coef_);
                archive->try_read("ent_coef_tensor", ent_coef_tensor_);
            }
        }

        virtual void SaveArchive(torch::serialize::OutputArchive* archive, const std::unordered_set<std::string>& names) override {
            if (names.count("all") || names.count("actor")) {
                torch::serialize::OutputArchive actor_archive;
                actor_->save(actor_archive);
                archive->write("actor", actor_archive);
            }
            if (names.count("all") || names.count("critic")) {
                torch::serialize::OutputArchive critic_archive;
                critic_->save(critic_archive);
                archive->write("critic", critic_archive);
            }
            if (names.count("all") || names.count("critic_target")) {
                torch::serialize::OutputArchive critic_target_archive;
                critic_target_->save(critic_target_archive);
                archive->write("critic_target", critic_target_archive);
            }
            if (names.count("all") || names.count("actor_optimizer")) {
                torch::serialize::OutputArchive actor_optimizer_archive;
                actor_optimizer_->save(actor_optimizer_archive);
                archive->write("actor_optimizer", actor_optimizer_archive);
            }
            if (names.count("all") || names.count("critic_optimizer")) {
                torch::serialize::OutputArchive critic_optimizer_archive;
                critic_optimizer_->save(critic_optimizer_archive);
                archive->write("critic_optimizer", critic_optimizer_archive);
            }
            if ((names.count("all") || names.count("ent_coef_optimizer")) && ent_coef_optimizer_ != nullptr) {
                torch::serialize::OutputArchive ent_coef_optimizer_archive;
                ent_coef_optimizer_->save(ent_coef_optimizer_archive);
                archive->write("ent_coef_optimizer", ent_coef_optimizer_archive);
            }
            if (names.count("all") || names.count("hparams")) {
                archive->write("learning_starts", torch::tensor(learning_starts_));
                archive->write("batch_size", torch::tensor(batch_size_));
                archive->write("lr", torch::tensor(lr_));
                archive->write("tau", torch::tensor(tau_));
                archive->write("gamma", torch::tensor(gamma_));
                archive->write("ent_coef", torch::tensor(ent_coef_));
                archive->write("target_entropy", torch::tensor(target_entropy_));
                archive->write("auto_ent_coef", torch::tensor(auto_ent_coef_));
                archive->write("train_freq", torch::tensor(train_freq_));
                archive->write("gradient_steps", torch::tensor(gradient_steps_));
                archive->write("target_update_interval", torch::tensor(target_update_interval_));
                if (log_ent_coef_.defined())
                    archive->write("log_ent_coef", log_ent_coef_);
                if (ent_coef_tensor_.defined())
                    archive->write("ent_coef_tensor", ent_coef_tensor_);
            }
        }

        virtual std::unique_ptr<torch::optim::Optimizer> MakeEntropyOptimizer() const {
            return std::make_unique<torch::optim::Adam>(std::vector<torch::Tensor>{log_ent_coef_}, torch::optim::AdamOptions(lr_));
        }

        virtual std::unique_ptr<torch::optim::Optimizer> MakeActorOptimizer() const {
            return std::make_unique<torch::optim::Adam>(actor_->parameters(), torch::optim::AdamOptions(lr_));
        }

        virtual std::unique_ptr<torch::optim::Optimizer> MakeCriticOptimizer() const {
            return std::make_unique<torch::optim::Adam>(critic_->parameters(), torch::optim::AdamOptions(lr_));
        }
        
        const std::unique_ptr<ReplayBuffer>& replay_buffer() const {
            return replay_buffer_;
        }

        const std::unique_ptr<SACActor>& actor() const {
            return actor_;
        }

        const std::unique_ptr<SACCritic>& critic() const {
            return critic_;
        }

        const std::unique_ptr<SACCritic>& critic_target() const {
            return critic_target_;
        }

        const std::unique_ptr<torch::optim::Optimizer>& actor_optimizer() const {
            return actor_optimizer_;
        }

        const std::unique_ptr<torch::optim::Optimizer>& critic_optimizer() const {
            return critic_optimizer_;
        }

    protected:
        Int learning_starts_;
        Int batch_size_;
        double lr_;
        double tau_;
        double gamma_;
        double ent_coef_;
        double target_entropy_;
        bool auto_ent_coef_;
        bool auto_target_entropy_;
        Int train_freq_;
        Int gradient_steps_;
        Int target_update_interval_;
        std::unique_ptr<ReplayBuffer> replay_buffer_ = nullptr;
        std::unique_ptr<SACActor> actor_ = nullptr;
        std::unique_ptr<SACCritic> critic_ = nullptr;
        std::unique_ptr<SACCritic> critic_target_ = nullptr;
        std::unique_ptr<torch::optim::Optimizer> actor_optimizer_ = nullptr;
        std::unique_ptr<torch::optim::Optimizer> critic_optimizer_ = nullptr;
        std::unique_ptr<torch::optim::Optimizer> ent_coef_optimizer_ = nullptr;
        torch::Tensor log_ent_coef_;
        torch::Tensor ent_coef_tensor_;
        torch::Tensor observation_;
    };
}