import time
import gymnasium as gym
from stable_baselines3 import PPO
from stable_baselines3.common.vec_env import DummyVecEnv, SubprocVecEnv
from stable_baselines3.common.env_util import make_vec_env
from stable_baselines3.common.evaluation import evaluate_policy
from stable_baselines3.common.utils import set_random_seed
from typing import Callable

if __name__ == '__main__':
    env_id = "LunarLander-v2"
    num_cpu = 16
    n_timesteps = 1e6
    n_experiments = 50
    path = 'data/ppo/lunar_lander/sb3'
    
    set_random_seed(0)
    
    with open(path + '_eval.txt', 'w') as f:
        pass
    
    for i in range(n_experiments):
        env = make_vec_env(env_id, seed=i, n_envs=num_cpu)

        model = PPO("MlpPolicy", env, verbose=1,
                learning_rate=3e-4,
                n_steps=1024,
                batch_size=64,
                n_epochs=4,
                gamma=0.99,
                gae_lambda=0.98,
                clip_range=0.2,
                clip_range_vf=None,
                normalize_advantage=False,
                ent_coef=0.01,
                vf_coef=0.1,
                max_grad_norm=0.5,
                target_kl=None,
                device='cuda',
                tensorboard_log=path
                )
        start_time = time.time()
        model.learn(total_timesteps=n_timesteps)
        duration = time.time() - start_time
        # model.save(path + '_' + str(i) +  '.pth')
        
        mean_reward, std_reward = evaluate_policy(model, env, n_eval_episodes=100)
        with open(path + '_eval.txt', 'a') as f:
            print(str(mean_reward) + '\t' + str(std_reward) + '\t' + str(duration), file=f)
        env.close()
